#include "aetherfs.h"
#include <linux/workqueue.h>
#include <linux/radix-tree.h>

#define AETHERFS_DEDUP_HASH_BITS    16
#define AETHERFS_DEDUP_HASH_SIZE    (1 << AETHERFS_DEDUP_HASH_BITS)
#define AETHERFS_DEDUP_MAX_CHUNK    65536
#define AETHERFS_DEDUP_MIN_CHUNK    4096
#define AETHERFS_CDC_WINDOW_SIZE    48

enum aetherfs_dedup_mode {
	DEDUP_MODE_NONE = 0,
	DEDUP_MODE_REFLINK = 1,
	DEDUP_MODE_BLOCK = 2,
	DEDUP_MODE_CDC = 3,
};

struct aetherfs_dedup_block {
	uint64_t db_blocknr;
	uint32_t db_checksum;
	uint32_t db_refcount;
	uint64_t db_refs[AETHERFS_DEDUP_HASH_SIZE];
};

struct aetherfs_dedup_info {
	enum aetherfs_dedup_mode dd_mode;
	atomic_t dd_enabled;
	atomic_t dd_running;
	uint64_t dd_saved_bytes;
	uint64_t dd_dedup_blocks;
	struct workqueue_struct *dd_wq;
	struct work_struct dd_scrub_work;
	void *dd_hash_table;
	rwlock_t dd_lock;
};

struct aetherfs_cdc_chunk {
	__le32 cc_checksum;
	__le32 cc_length;
	__le64 cc_ref_block;
	__le32 cc_ref_count;
	__le32 cc_reserved;
};

struct aetherfs_dedup_hint {
	__le64 dh_ino;
	__le64 dh_offset;
	__le64 dh_length;
	__le32 dh_checksum;
	__le32 dh_flags;
#define DEDUP_HINT_F_SHARED    0x00000001
#define DEDUP_HINT_F_COMPRESS  0x00000002
#define DEDUP_HINT_F_SPARSE    0x00000004
};

static struct aetherfs_dedup_info global_dedup;
static DECLARE_RADIX_TREE(dedup_tree, GFP_NOFS);

int aetherfs_reflink(struct inode *src_inode, loff_t src_pos,
		 struct inode *dst_inode, loff_t dst_pos,
		 loff_t len, int preserve)
{
	struct super_block *sb = src_inode->i_sb;
	struct aetherfs_inode_info *dei;
	sector_t src_blk, dst_blk;
	loff_t copied = 0;

	if (!src_inode || !dst_inode || sb != dst_inode->i_sb)
		return -EINVAL;

	dei = AETHERFS_INODE(dst_inode);

	while (copied < len) {
		src_blk = (src_pos + copied) / sb->s_blocksize;
		dst_blk = (dst_pos + copied) / sb->s_blocksize;

		dei->i_flags |= cpu_to_le32(AETHERFS_F_SHARED_EXTENT);

		copied += sb->s_blocksize;
	}

	if (preserve)
		dei->i_flags |= cpu_to_le32(AETHERFS_F_REFLINKED);

	mark_inode_dirty(src_inode);
	mark_inode_dirty(dst_inode);

	return 0;
}

int aetherfs_clone(struct inode *src, struct dentry *dst_dentry)
{
	struct inode *dst;
	struct inode *parent = d_inode(dentry_parent(dst_dentry));
	int err;

	if (!src || !dst_dentry)
		return -EINVAL;

	dst = aetherfs_new_inode(src->i_sb, src->i_mode, 0);
	if (IS_ERR(dst))
		return PTR_ERR(dst);

	err = aetherfs_reflink(src, 0, dst, 0, i_size_read(src), 1);
	if (err) {
		iput(dst);
		return err;
	}

	i_size_write(dst, i_size_read(src));
	d_instantiate(dst_dentry, dst);

	return 0;
}

int aetherfs_dedupe_range(struct inode *inode, loff_t dst_off,
			struct inode *src_inode, loff_t src_off,
			loff_t len)
{
	struct super_block *sb = inode->i_sb;
	struct aetherfs_inode_info *dei;

	if (!inode || !src_inode)
		return -EINVAL;

	if (sb != src_inode->i_sb)
		return -EXDEV;

	dei = AETHERFS_INODE(inode);
	dei->i_flags |= cpu_to_le32(AETHERFS_F_SHARED_EXTENT);

	return aetherfs_reflink(src_inode, src_off, inode, dst_off, len, 0);
}

int aetherfs_is_shared_extent(struct inode *inode, loff_t offset)
{
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);

	if (!inode)
		return 0;

	return (le32_to_cpu(ei->i_flags) & AETHERFS_F_SHARED_EXTENT) != 0;
}

static uint32_t block_checksum(void *data, uint32_t size)
{
	return aetherfs_crc32c(AETHERFS_CRC_SEED, data, size);
}

int aetherfs_dedup_enable(enum aetherfs_dedup_mode mode)
{
	if (mode > DEDUP_MODE_CDC)
		return -EINVAL;

	global_dedup.dd_mode = mode;
	atomic_set(&global_dedup.dd_enabled, 1);

	pr_info("AetherFS: dedup enabled in mode %d\n", mode);
	return 0;
}

void aetherfs_dedup_disable(void)
{
	atomic_set(&global_dedup.dd_enabled, 0);
	global_dedup.dd_mode = DEDUP_MODE_NONE;

	pr_info("AetherFS: dedup disabled\n");
}

int aetherfs_dedup_block(struct super_block *sb, uint64_t blocknr,
			 uint32_t checksum, void *data)
{
	void *entry;
	int ret;

	if (!sb || !blocknr || !data)
		return -EINVAL;

	if (!atomic_read(&global_dedup.dd_enabled))
		return 0;

	if (global_dedup.dd_mode < DEDUP_MODE_BLOCK)
		return 0;

	write_lock(&global_dedup.dd_lock);

	entry = radix_tree_lookup(&dedup_tree, checksum);
	if (entry) {
		struct aetherfs_dedup_block *db = entry;
		db->db_refcount = cpu_to_le32(le32_to_cpu(db->db_refcount) + 1);
		global_dedup.dd_saved_bytes += sb->s_blocksize;
		global_dedup.dd_dedup_blocks++;
		ret = 0;
	} else {
		struct aetherfs_dedup_block *db;
		db = kzalloc(sizeof(*db), GFP_NOFS);
		if (!db) {
			ret = -ENOMEM;
			goto out;
		}
		db->db_blocknr = blocknr;
		db->db_checksum = checksum;
		db->db_refcount = cpu_to_le32(1);
		ret = radix_tree_insert(&dedup_tree, checksum, db);
		if (ret)
			kfree(db);
	}

out:
	write_unlock(&global_dedup.dd_lock);
	return ret;
}

int aetherfs_dedup_resolve(struct super_block *sb, uint32_t checksum,
			   uint64_t *blocknr, uint32_t *refcount)
{
	void *entry;
	int ret = -ENOENT;

	if (!sb || !blocknr || !refcount)
		return -EINVAL;

	if (!atomic_read(&global_dedup.dd_enabled))
		return -EINVAL;

	read_lock(&global_dedup.dd_lock);

	entry = radix_tree_lookup(&dedup_tree, checksum);
	if (entry) {
		struct aetherfs_dedup_block *db = entry;
		*blocknr = db->db_blocknr;
		*refcount = le32_to_cpu(db->db_refcount);
		ret = 0;
	}

	read_unlock(&global_dedup.dd_lock);

	return ret;
}

int aetherfs_dedup_release(struct super_block *sb, uint32_t checksum)
{
	void *entry;
	int ret = -ENOENT;

	if (!sb || !checksum)
		return -EINVAL;

	write_lock(&global_dedup.dd_lock);

	entry = radix_tree_lookup(&dedup_tree, checksum);
	if (entry) {
		struct aetherfs_dedup_block *db = entry;
		uint32_t refs = le32_to_cpu(db->db_refcount) - 1;
		
		if (refs == 0) {
			radix_tree_delete(&dedup_tree, checksum);
			kfree(db);
		} else {
			db->db_refcount = cpu_to_le32(refs);
		}
		ret = 0;
	}

	write_unlock(&global_dedup.dd_lock);

	return ret;
}

static int cdc_find_chunk_boundary(const uint8_t *data, size_t size,
				   size_t *chunk_start, size_t *chunk_len)
{
	static const uint16_t cdc_table[256] = {
		0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE201,
		0xA001, 0x6C00, 0x7800, 0x9901, 0x8801, 0x7800, 0x6800, 0xB001,
		0x5001, 0x3C00, 0x4C00, 0x8501, 0x9401, 0xA001, 0x8400, 0xD001,
		0xD801, 0xC001, 0xFC01, 0x1400, 0x4C00, 0xCC01, 0xD801, 0x1400,
	};
	size_t i;
	uint16_t crc = 0xFFFF;
	uint32_t hash;

	for (i = 0; i < size - 1; i++) {
		uint8_t byte = data[i];
		uint16_t tmp = cdc_table[crc & 0xF];
		crc = (crc >> 4) ^ tmp ^ cdc_table[byte & 0xF];
		tmp = cdc_table[crc & 0xF];
		crc = (crc >> 4) ^ tmp ^ cdc_table[byte >> 4];
		hash = crc & 0x3FFF;

		if ((hash & 0x3FF) == 0x3FF && i >= AETHERFS_DEDUP_MIN_CHUNK) {
			*chunk_start = i - (AETHERFS_CDC_WINDOW_SIZE - 1);
			if (*chunk_start < 0)
				*chunk_start = 0;
			*chunk_len = i - *chunk_start;
			if (*chunk_len > AETHERFS_DEDUP_MAX_CHUNK)
				*chunk_len = AETHERFS_DEDUP_MAX_CHUNK;
			return 0;
		}
	}

	*chunk_start = 0;
	*chunk_len = size < AETHERFS_DEDUP_MIN_CHUNK ? size : AETHERFS_DEDUP_MIN_CHUNK;
	return 0;
}

int aetherfs_cdc_dedup(struct inode *inode, const void *data, size_t size,
		       uint64_t *chunk_refs, uint32_t chunk_count)
{
	size_t offset = 0;
	uint32_t i;

	if (!inode || !data || !size)
		return -EINVAL;

	if (global_dedup.dd_mode != DEDUP_MODE_CDC)
		return 0;

	for (i = 0; i < chunk_count && offset < size; i++) {
		size_t chunk_start, chunk_len;
		uint32_t checksum;

		cdc_find_chunk_boundary(data + offset, size - offset,
				       &chunk_start, &chunk_len);

		checksum = block_checksum(data + offset + chunk_start, chunk_len);

		aetherfs_dedup_block(inode->i_sb, 0, checksum,
				     data + offset + chunk_start);

		chunk_refs[i] = checksum;
		offset += chunk_start + chunk_len;
	}

	return 0;
}

int aetherfs_dedup_hint_set(struct inode *inode, loff_t offset, loff_t len,
			     uint32_t checksum, uint32_t flags)
{
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);
	struct aetherfs_dedup_hint hint;

	if (!inode || !len)
		return -EINVAL;

	hint.dh_ino = cpu_to_le64(inode->i_ino);
	hint.dh_offset = cpu_to_le64(offset);
	hint.dh_length = cpu_to_le64(len);
	hint.dh_checksum = cpu_to_le32(checksum);
	hint.dh_flags = cpu_to_le32(flags);

	if (flags & DEDUP_HINT_F_SHARED)
		ei->i_flags |= cpu_to_le32(AETHERFS_F_SHARED_EXTENT);

	mark_inode_dirty(inode);

	return 0;
}

int aetherfs_dedup_hint_get(struct inode *inode, loff_t offset,
			    struct aetherfs_dedup_hint *hint)
{
	if (!inode || !hint)
		return -EINVAL;

	hint->dh_ino = cpu_to_le64(inode->i_ino);
	hint->dh_offset = cpu_to_le64(offset);

	return 0;
}

int aetherfs_dedup_ioctl(struct inode *inode, unsigned int cmd,
			 unsigned long arg)
{
	struct aetherfs_dedup_info info;
	int ret;

	switch (cmd) {
	case 0:
		info.dd_mode = global_dedup.dd_mode;
		info.dd_saved_bytes = global_dedup.dd_saved_bytes;
		info.dd_dedup_blocks = global_dedup.dd_dedup_blocks;
		info.dd_enabled = atomic_read(&global_dedup.dd_enabled);
		ret = 0;
		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

uint64_t aetherfs_dedup_saved_bytes(void)
{
	return global_dedup.dd_saved_bytes;
}

uint64_t aetherfs_dedup_block_count(void)
{
	return global_dedup.dd_dedup_blocks;
}

int aetherfs_dedup_init(void)
{
	rwlock_init(&global_dedup.dd_lock);
	atomic_set(&global_dedup.dd_enabled, 0);
	atomic_set(&global_dedup.dd_running, 0);
	global_dedup.dd_mode = DEDUP_MODE_REFLINK;
	global_dedup.dd_saved_bytes = 0;
	global_dedup.dd_dedup_blocks = 0;

	INIT_RADIX_TREE(&dedup_tree, GFP_NOFS);

	pr_info("AetherFS: dedup module initialized (default: reflink mode)\n");
	return 0;
}

void aetherfs_dedup_exit(void)
{
	struct radix_tree_iter iter;
	void **slot;

	atomic_set(&global_dedup.dd_enabled, 0);

	radix_tree_for_each_slot(slot, &dedup_tree, &iter, 0) {
		kfree(*slot);
		radix_tree_delete(&dedup_tree, iter.index);
	}

	pr_info("AetherFS: dedup module exiting, saved %llu bytes in %llu blocks\n",
		global_dedup.dd_saved_bytes, global_dedup.dd_dedup_blocks);
}