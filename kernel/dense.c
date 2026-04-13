#include "aetherfs.h"

#define AETHERFS_INLINE_FILE_MAX     4096
#define AETHERFS_TAIL_PACKING_THRESHOLD  4096
#define AETHERFS_TAIL_PACKING_MIN        512

#define AETHERFS_PACKED_XATTR_MAX   4096
#define AETHERFS_XATTR_INLINE_MAX   256

#define AETHERFS_DIR_ENTRY_MIN      8
#define AETHERFS_DIR_ENTRY_MAX      255
#define AETHERFS_DIR_ENTRY_INLINE   16

#define AETHERFS_SYMLINK_INLINE_MAX 256
#define AETHERFS_DEDUP_HASH_BITS    12
#define AETHERFS_DEDUP_HASH_SIZE    (1 << AETHERFS_DEDUP_HASH_BITS)

struct aetherfs_inline_data {
	__le32 id_magic;
	__le32 id_size;
	__le32 id_checksum;
	__le16 id_refcount;
	__le16 id_reserved;
	__u8   id_data[];
};

struct aetherfs_tail_packed {
	__le32 tp_magic;
	__le32 tp_size;
	__le32 tp_orig_offset;
	__le32 tp_checksum;
	__u8   tp_data[];
};

struct aetherfs_packed_xattr {
	__le16 px_flags;
	__le16 px_name_len;
	__le16 px_value_len;
	__le16 px_next;
	__le32 px_checksum;
	__u8   px_name[];
};

struct aetherfs_compact_dir_entry {
	__le32 cde_hash;
	__le64 cde_ino;
	__le16 cde_namelen;
	__le8  cde_type;
	__le8  cde_flags;
	__le16 cde_next;
};

struct aetherfs_dedup_entry {
	__le32 de_hash;
	__le32 de_refcount;
	__le32 de_size;
	__le32 de_checksum;
	__le64 de_first_ino;
	__le64 de_next;
	__u8   de_data[];
};

static struct kmem_cache *inline_data_cache;
static struct kmem_cache *xattr_cache;
static struct kmem_cache *dedup_cache;
static void *dedup_hash_table[AETHERFS_DEDUP_HASH_SIZE];

int aetherfs_small_file_init(void)
{
	inline_data_cache = kmem_cache_create("aetherfs_inline",
						sizeof(struct aetherfs_inline_data) + 
						AETHERFS_INLINE_FILE_MAX,
						0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!inline_data_cache)
		return -ENOMEM;

	xattr_cache = kmem_cache_create("aetherfs_xattr",
					 sizeof(struct aetherfs_packed_xattr) + 256,
					 0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!xattr_cache) {
		kmem_cache_destroy(inline_data_cache);
		return -ENOMEM;
	}

	dedup_cache = kmem_cache_create("aetherfs_dedup",
					 sizeof(struct aetherfs_dedup_entry) + 128,
					 0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!dedup_cache) {
		kmem_cache_destroy(inline_data_cache);
		kmem_cache_destroy(xattr_cache);
		return -ENOMEM;
	}

	pr_info("AetherFS: small file support initialized (inline max: %d bytes)\n",
		AETHERFS_INLINE_FILE_MAX);
	return 0;
}

void aetherfs_small_file_exit(void)
{
	if (inline_data_cache)
		kmem_cache_destroy(inline_data_cache);
	if (xattr_cache)
		kmem_cache_destroy(xattr_cache);
	if (dedup_cache)
		kmem_cache_destroy(dedup_cache);
}

int aetherfs_should_store_inline(struct inode *inode, uint64_t size)
{
	if (!inode || !S_ISREG(inode->i_mode))
		return 0;

	if (size <= AETHERFS_INLINE_FILE_MAX)
		return 1;

	return 0;
}

int aetherfs_write_inline_file(struct inode *inode, const void *data, uint64_t size)
{
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);
	struct buffer_head *bh;
	struct aetherfs_inode *raw;
	uint32_t checksum;
	int ret;

	if (!inode || !data || size > AETHERFS_INLINE_FILE_MAX)
		return -EINVAL;

	bh = sb_bread(inode->i_sb, inode->i_ino);
	if (!bh)
		return -EIO;

	raw = (struct aetherfs_inode *)bh->b_data;

	raw->i_flags |= cpu_to_le32(AETHERFS_F_INLINE);
	raw->i_inline_len = cpu_to_le32((uint32_t)size);

	memcpy(raw->i_inline, data, size);

	checksum = aetherfs_crc32c(AETHERFS_CRC_SEED, data, size);
	*((uint32_t *)(raw->i_inline + size)) = cpu_to_le32(checksum);

	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	i_size_write(inode, size);
	mark_inode_dirty(inode);

	return 0;
}

int aetherfs_read_inline_file(struct inode *inode, void *data, uint64_t *size)
{
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);
	struct buffer_head *bh;
	struct aetherfs_inode *raw;
	uint32_t stored_checksum, computed_checksum;
	uint32_t inline_len;

	if (!inode || !data || !size)
		return -EINVAL;

	if (!(le32_to_cpu(ei->i_flags) & AETHERFS_F_INLINE))
		return -EINVAL;

	bh = sb_bread(inode->i_sb, inode->i_ino);
	if (!bh)
		return -EIO;

	raw = (struct aetherfs_inode *)bh->b_data;
	inline_len = le32_to_cpu(raw->i_inline_len);

	if (inline_len > AETHERFS_INLINE_FILE_MAX) {
		brelse(bh);
		return -EIO;
	}

	memcpy(data, raw->i_inline, inline_len);

	stored_checksum = *((uint32_t *)(raw->i_inline + inline_len));
	computed_checksum = aetherfs_crc32c(AETHERFS_CRC_SEED, data, inline_len);

	brelse(bh);

	if (stored_checksum != cpu_to_le32(computed_checksum)) {
		pr_warn("AetherFS: inline file checksum mismatch\n");
		return -EUCLEAN;
	}

	*size = inline_len;
	return 0;
}

int aetherfs_tail_pack(struct inode *inode, uint64_t offset, const void *data, uint32_t len)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct aetherfs_tail_packed *tail;
	uint64_t block;
	uint32_t checksum;
	int ret;

	if (!inode || !data || !len)
		return -EINVAL;

	if (len < AETHERFS_TAIL_PACKING_MIN)
		return -EINVAL;

	if (len > sb->s_blocksize - sizeof(struct aetherfs_tail_packed))
		return -EINVAL;

	block = aetherfs_alloc_blocks(inode, 0, 1);
	if (!block)
		return -ENOSPC;

	bh = sb_getblk(sb, block);
	if (!bh) {
		aetherfs_free_blocks(sb, block, 1);
		return -EIO;
	}

	tail = (struct aetherfs_tail_packed *)bh->b_data;
	tail->tp_magic = cpu_to_le32(0x5441494C);
	tail->tp_size = cpu_to_le32(len);
	tail->tp_orig_offset = cpu_to_le32((uint32_t)offset);
	memcpy(tail->tp_data, data, len);

	checksum = aetherfs_crc32c(AETHERFS_CRC_SEED, tail, 
		sizeof(*tail) + len - sizeof(__le32));
	tail->tp_checksum = cpu_to_le32(checksum);

	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int aetherfs_tail_unpack(struct inode *inode, uint64_t offset, void *data, uint32_t *len)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct aetherfs_tail_packed *tail;
	uint64_t block = 0;
	uint32_t stored_checksum, computed_checksum;
	int ret;

	if (!inode || !data || !len)
		return -EINVAL;

	ret = aetherfs_btree_lookup_extent(NULL, inode->i_ino, offset, &block, len);
	if (ret || !block)
		return -ENOENT;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;

	tail = (struct aetherfs_tail_packed *)bh->b_data;

	if (le32_to_cpu(tail->tp_magic) != 0x5441494C) {
		brelse(bh);
		return -EIO;
	}

	*len = le32_to_cpu(tail->tp_size);
	if (*len > sb->s_blocksize - sizeof(struct aetherfs_tail_packed)) {
		brelse(bh);
		return -EIO;
	}

	stored_checksum = le32_to_cpu(tail->tp_checksum);
	tail->tp_checksum = 0;
	computed_checksum = aetherfs_crc32c(AETHERFS_CRC_SEED, tail,
		sizeof(*tail) + *len - sizeof(__le32));
	tail->tp_checksum = cpu_to_le32(stored_checksum);

	if (stored_checksum != computed_checksum) {
		brelse(bh);
		return -EUCLEAN;
	}

	memcpy(data, tail->tp_data, *len);
	brelse(bh);

	return 0;
}

int aetherfs_is_tail_packing_worth(uint32_t tail_size, uint32_t block_size)
{
	uint32_t overhead = sizeof(struct aetherfs_tail_packed);
	uint32_t saved = block_size - (tail_size + overhead);
	
	return saved >= (block_size / 8);
}

int aetherfs_write_packed_xattr(struct inode *inode, const char *name,
				const void *value, uint32_t valuelen)
{
	struct super_block *sb = inode->i_sb;
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);
	struct buffer_head *bh;
	struct aetherfs_packed_xattr *xattr;
	size_t name_len = strlen(name);
	uint32_t total_size, checksum;
	int ret;

	if (!inode || !name || !value || !valuelen)
		return -EINVAL;

	if (name_len > 255 || valuelen > AETHERFS_PACKED_XATTR_MAX)
		return -EINVAL;

	total_size = sizeof(struct aetherfs_packed_xattr) + name_len + valuelen;

	if (total_size > AETHERFS_XATTR_INLINE_MAX) {
		return -ENOSPC;
	}

	bh = sb_bread(sb, inode->i_ino);
	if (!bh)
		return -EIO;

	xattr = (struct aetherfs_packed_xattr *)((char *)bh->b_data + 128);
	
	xattr->px_name_len = cpu_to_le16(name_len);
	xattr->px_value_len = cpu_to_le16(valuelen);
	xattr->px_flags = 0;
	xattr->px_next = 0;

	memcpy(xattr->px_name, name, name_len);
	memcpy(xattr->px_name + name_len, value, valuelen);

	checksum = aetherfs_crc32c(AETHERFS_CRC_SEED, xattr, 
		sizeof(*xattr) + name_len + valuelen - sizeof(__le32));
	xattr->px_checksum = cpu_to_le32(checksum);

	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int aetherfs_read_packed_xattr(struct inode *inode, const char *name,
			       void *value, uint32_t *valuelen)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct aetherfs_packed_xattr *xattr;
	size_t name_len = strlen(name);
	uint32_t stored_checksum, computed_checksum;
	int ret;

	if (!inode || !name || !value || !valuelen)
		return -EINVAL;

	bh = sb_bread(sb, inode->i_ino);
	if (!bh)
		return -EIO;

	xattr = (struct aetherfs_packed_xattr *)((char *)bh->b_data + 128);

	if (le16_to_cpu(xattr->px_name_len) != name_len ||
	    memcmp(xattr->px_name, name, name_len) != 0) {
		brelse(bh);
		return -ENOENT;
	}

	stored_checksum = le32_to_cpu(xattr->px_checksum);
	xattr->px_checksum = 0;
	computed_checksum = aetherfs_crc32c(AETHERFS_CRC_SEED, xattr,
		sizeof(*xattr) + name_len + le16_to_cpu(xattr->px_value_len) - sizeof(__le32));
	xattr->px_checksum = cpu_to_le32(stored_checksum);

	if (stored_checksum != computed_checksum) {
		brelse(bh);
		return -EUCLEAN;
	}

	*valuelen = le16_to_cpu(xattr->px_value_len);
	memcpy(value, xattr->px_name + name_len, *valuelen);

	brelse(bh);
	return 0;
}

int aetherfs_symlink_inline(struct inode *inode, const char *target)
{
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);
	size_t len = strlen(target);

	if (!inode || !target)
		return -EINVAL;

	if (len > AETHERFS_SYMLINK_INLINE_MAX)
		return -ENAMETOOLONG;

	i_size_write(inode, len);
	inode->i_blocks = 0;

	ei->i_flags |= AETHERFS_F_INLINE;
	mark_inode_dirty(inode);

	return 0;
}

int aetherfs_symlink_read(struct inode *inode, char *target)
{
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);
	uint64_t size;

	if (!inode || !target)
		return -EINVAL;

	if (le32_to_cpu(ei->i_flags) & AETHERFS_F_INLINE) {
		size = i_size_read(inode);
		if (size > AETHERFS_SYMLINK_INLINE_MAX)
			return -EIO;
		return -EOPNOTSUPP;
	}

	return -EOPNOTSUPP;
}

int aetherfs_encode_dir_entry(struct aetherfs_dir_entry *de,
			       const char *name, size_t namelen,
			       uint64_t ino, uint8_t type)
{
	uint32_t hash;

	if (!de || !name || !namelen || !ino)
		return -EINVAL;

	if (namelen > AETHERFS_DIR_ENTRY_MAX)
		return -ENAMETOOLONG;

	hash = aetherfs_hash_name(name, namelen);

	de->inode = cpu_to_le64(ino);
	de->rec_len = cpu_to_le16(AETHERFS_DIR_ENTRY_MIN + namelen);
	de->name_len = cpu_to_le8(namelen);
	de->file_type = type;
	de->checksum = 0;

	memcpy(de->name, name, namelen);

	de->checksum = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, de,
			sizeof(*de) - sizeof(__le32) + namelen));

	return 0;
}

int aetherfs_compact_dir_entry(struct aetherfs_dir_entry *src,
				struct aetherfs_compact_dir_entry *dst)
{
	uint32_t hash;

	if (!src || !dst)
		return -EINVAL;

	hash = aetherfs_hash_name(src->name, src->name_len);

	dst->cde_hash = cpu_to_le32(hash);
	dst->cde_ino = src->inode;
	dst->cde_namelen = src->name_len;
	dst->cde_type = src->file_type;
	dst->cde_flags = 0;
	dst->cde_next = 0;

	return 0;
}

static inline uint32_t dedup_hash(const void *data, uint32_t size)
{
	return aetherfs_crc32c(AETHERFS_CRC_SEED, data, size) & (AETHERFS_DEDUP_HASH_SIZE - 1);
}

int aetherfs_dedup_metadata(struct inode *inode, void *data, uint32_t size)
{
	struct aetherfs_dedup_entry *entry;
	uint32_t hash;
	void **bucket;

	if (!inode || !data || !size || size > 128)
		return -EINVAL;

	hash = dedup_hash(data, size);
	bucket = &dedup_hash_table[hash];

	if (*bucket) {
		entry = *(struct aetherfs_dedup_entry **)bucket;
		while (entry) {
			if (le32_to_cpu(entry->de_size) == size &&
			    memcmp(entry->de_data, data, size) == 0) {
				entry->de_refcount = cpu_to_le32(
					le32_to_cpu(entry->de_refcount) + 1);
				pr_info("AetherFS: dedup hit refcount=%u\n",
					le32_to_cpu(entry->de_refcount));
				return 0;
			}
			entry = (void *)(unsigned long)le64_to_cpu(entry->de_next);
		}
	}

	entry = kmem_cache_zalloc(dedup_cache, GFP_NOFS);
	if (!entry)
		return -ENOMEM;

	entry->de_hash = cpu_to_le32(hash);
	entry->de_refcount = cpu_to_le32(1);
	entry->de_size = cpu_to_le32(size);
	entry->de_first_ino = cpu_to_le64(inode->i_ino);
	entry->de_next = 0;

	memcpy(entry->de_data, data, size);

	entry->de_checksum = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, entry, 
			sizeof(*entry) + size - sizeof(__le32)));

	entry->de_next = cpu_to_le64((unsigned long)*bucket);
	*bucket = entry;

	return 0;
}

int aetherfs_dedup_resolve(struct inode *inode, void *data, uint32_t size)
{
	struct aetherfs_dedup_entry *entry;
	uint32_t hash;
	void **bucket;

	if (!inode || !data || !size)
		return -EINVAL;

	hash = dedup_hash(data, size);
	bucket = &dedup_hash_table[hash];

	entry = *(struct aetherfs_dedup_entry **)bucket;
	while (entry) {
		if (le32_to_cpu(entry->de_size) == size &&
		    memcmp(entry->de_data, data, size) == 0) {
			memcpy(data, entry->de_data, size);
			return 0;
		}
		entry = (void *)(unsigned long)le64_to_cpu(entry->de_next);
	}

	return -ENOENT;
}

int aetherfs_dedup_release(struct inode *inode, void *data, uint32_t size)
{
	struct aetherfs_dedup_entry *entry, **prev;
	uint32_t hash;
	void **bucket;

	if (!inode || !data || !size)
		return -EINVAL;

	hash = dedup_hash(data, size);
	bucket = &dedup_hash_table[hash];
	prev = (struct aetherfs_dedup_entry **)bucket;

	entry = *prev;
	while (entry) {
		if (le32_to_cpu(entry->de_size) == size &&
		    memcmp(entry->de_data, data, size) == 0) {
			uint32_t refs = le32_to_cpu(entry->de_refcount) - 1;
			entry->de_refcount = cpu_to_le32(refs);
			if (refs == 0) {
				*prev = (void *)(unsigned long)entry->de_next;
				kmem_cache_free(dedup_cache, entry);
			}
			return 0;
		}
		prev = (struct aetherfs_dedup_entry **)&entry->de_next;
		entry = (void *)(unsigned long)le64_to_cpu(entry->de_next);
	}

	return -ENOENT;
}