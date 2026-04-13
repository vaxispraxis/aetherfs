#include "aetherfs.h"
#include <linux/dcache.h>
#include <linux/string.h>
#include <linux/ctype.h>

#define AETHERFS_DIR_INDEX_DEPTH_MAX    3
#define AETHERFS_DIR_HTREE_ORDER       32
#define AETHERFS_DIR_LEAF_ENTRIES      128
#define AETHERFS_DIR_HASH_BITS         16
#define AETHERFS_DIR_ENTRIES_PER_BLOCK 64

#define AETHERFS_DIR_CASE_SENSITIVE    0
#define AETHERFS_DIR_CASE_NORMALIZED   1

enum aetherfs_dir_index_type {
	DIR_INDEX_HASH = 0,
	DIR_INDEX_ORDERED = 1,
	DIR_INDEX_BOTH = 2,
};

struct aetherfs_dir_htree {
	__le64 h_root_block;
	__le32 h_depth;
	__le32 h_entries;
	__le32 h_hash_seed;
	__le32 h_type;
	__le64 h_ordered_root;
	__le32 h_split_threshold;
	__le32 h_reserved;
};

struct aetherfs_dir_hash_entry {
	__le32 dhe_hash;
	__le32 dhe_next;
	__le64 dhe_ino;
	__le16 dhe_namelen;
	__le16 dhe_flags;
	__le32 dhe_checksum;
	char dhe_name[];
};

struct aetherfs_dir_leaf {
	__le32 dl_type;
	__le32 dl_count;
	__le32 dl_flags;
	__le32 dl_checksum;
	__le64 dl_next_leaf;
	struct aetherfs_dir_hash_entry dl_entries[];
};

struct aetherfs_dir_internal {
	__le32 di_type;
	__le32 di_count;
	__le32 di_flags;
	__le32 di_checksum;
	struct {
		__le32 di_hash;
		__le64 di_child;
	} di_entries[];
};

struct aetherfs_dir_ordered_index {
	__le32 oi_type;
	__le32 oi_count;
	__le32 oi_checksum;
	__le64 oi_next;
	struct {
		__le32 oi_len;
		char   oi_name[];
	} oi_entries[];
};

static uint32_t dir_hash_seed = 0x12345678;

static inline uint32_t aetherfs_hash_name(const char *name, size_t namelen)
{
	uint32_t hash = dir_hash_seed;
	size_t i;

	for (i = 0; i < namelen; i++) {
		hash = (hash << 5) + hash + (uint32_t)name[i];
		hash ^= (hash >> 16);
	}

	hash = (hash >> 11) ^ (hash << 5);
	return hash & ((1 << AETHERFS_DIR_HASH_BITS) - 1);
}

static inline int aetherfs_hash_match(uint32_t h1, uint32_t h2)
{
	return h1 == h2;
}

int aetherfs_dir_index_init(struct inode *dir)
{
	struct aetherfs_sb_info *info = AETH_SB(dir->i_sb);
	struct buffer_head *bh;
	struct aetherfs_dir_htree *htree;

	if (!dir || !S_ISDIR(dir->i_mode))
		return -EINVAL;

	bh = sb_bread(dir->i_sb, dir->i_ino);
	if (!bh)
		return -EIO;

	htree = (struct aetherfs_dir_htree *)bh->b_data;
	htree->h_root_block = cpu_to_le64(0);
	htree->h_depth = cpu_to_le32(0);
	htree->h_entries = cpu_to_le32(0);
	htree->h_hash_seed = cpu_to_le32(dir_hash_seed);
	htree->h_type = cpu_to_le32(DIR_INDEX_HASH);
	htree->h_ordered_root = cpu_to_le64(0);
	htree->h_split_threshold = cpu_to_le32(AETHERFS_DIR_LEAF_ENTRIES);

	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	info->s_dirty = 1;
	return 0;
}

int aetherfs_dir_htree_insert(struct inode *dir, const char *name, size_t namelen,
			       uint64_t ino, uint32_t type)
{
	struct super_block *sb = dir->i_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	uint32_t hash = aetherfs_hash_name(name, namelen);
	struct buffer_head *bh, *leaf_bh;
	struct aetherfs_dir_htree *htree;
	struct aetherfs_dir_leaf *leaf;
	struct aetherfs_dir_hash_entry *entry;
	size_t entry_size;
	uint64_t leaf_block;
	int ret;

	if (!dir || !name || !ino)
		return -EINVAL;

	if (namelen > AETHERFS_MAX_NAMELEN)
		return -ENAMETOOLONG;

	bh = sb_bread(sb, dir->i_ino);
	if (!bh)
		return -EIO;

	htree = (struct aetherfs_dir_htree *)bh->b_data;
	leaf_block = le64_to_cpu(htree->h_root_block);

	if (leaf_block == 0) {
		leaf_block = aetherfs_alloc_blocks(dir, 0, 1);
		if (!leaf_block) {
			brelse(bh);
			return -ENOSPC;
		}
		htree->h_root_block = cpu_to_le64(leaf_block);
		htree->h_depth = cpu_to_le32(0);
		set_buffer_dirty(bh);
	}

	brelse(bh);

	leaf_bh = sb_getblk(sb, leaf_block);
	if (!leaf_bh)
		return -EIO;

	leaf = (struct aetherfs_dir_leaf *)leaf_bh->b_data;

	if (le32_to_cpu(leaf->dl_type) == 0) {
		leaf->dl_type = cpu_to_le32(DIR_INDEX_HASH);
		leaf->dl_count = cpu_to_le32(0);
		leaf->dl_next_leaf = cpu_to_le64(0);
	}

	if (le32_to_cpu(leaf->dl_count) >= AETHERFS_DIR_LEAF_ENTRIES) {
		uint64_t new_leaf = aetherfs_alloc_blocks(dir, 0, 1);
		if (!new_leaf) {
			brelse(leaf_bh);
			return -ENOSPC;
		}
		leaf->dl_next_leaf = cpu_to_le64(new_leaf);
		leaf = (struct aetherfs_dir_leaf *)((char *)leaf_bh->b_data);
		leaf->dl_type = cpu_to_le32(DIR_INDEX_HASH);
		leaf->dl_count = cpu_to_le32(0);
	}

	entry_size = sizeof(struct aetherfs_dir_hash_entry) + namelen;
	entry = (struct aetherfs_dir_hash_entry *)((char *)leaf->dl_entries +
		le32_to_cpu(leaf->dl_count) * entry_size);

	entry->dhe_hash = cpu_to_le32(hash);
	entry->dhe_ino = cpu_to_le64(ino);
	entry->dhe_namelen = cpu_to_le16(namelen);
	entry->dhe_flags = cpu_to_le16(type);
	entry->dhe_checksum = cpu_to_le32(
		aetherfs_crc32(entry, entry_size - 4));
	memcpy(entry->dhe_name, name, namelen);

	leaf->dl_count = cpu_to_le32(le32_to_cpu(leaf->dl_count) + 1);
	leaf->dl_checksum = cpu_to_le32(
		aetherfs_crc32(leaf, sb->s_blocksize - 4));

	set_buffer_dirty(leaf_bh);
	sync_dirty_buffer(leaf_bh);
	brelse(leaf_bh);

	htree = NULL;
	bh = sb_bread(sb, dir->i_ino);
	if (bh) {
		htree = (struct aetherfs_dir_htree *)bh->b_data;
		htree->h_entries = cpu_to_le32(le32_to_cpu(htree->h_entries) + 1);
		set_buffer_dirty(bh);
		brelse(bh);
	}

	return 0;
}

int aetherfs_dir_htree_lookup(struct inode *dir, const char *name, size_t namelen,
			       uint64_t *ino)
{
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh, *leaf_bh;
	struct aetherfs_dir_htree *htree;
	struct aetherfs_dir_leaf *leaf;
	struct aetherfs_dir_hash_entry *entry;
	uint32_t hash = aetherfs_hash_name(name, namelen);
	uint64_t leaf_block;
	uint32_t count, i;
	size_t entry_size;
	int ret = -ENOENT;

	if (!dir || !name || !ino)
		return -EINVAL;

	bh = sb_bread(sb, dir->i_ino);
	if (!bh)
		return -EIO;

	htree = (struct aetherfs_dir_htree *)bh->b_data;
	leaf_block = le64_to_cpu(htree->h_root_block);
	brelse(bh);

	if (leaf_block == 0)
		return -ENOENT;

	leaf_bh = sb_getblk(sb, leaf_block);
	if (!leaf_bh)
		return -EIO;

	leaf = (struct aetherfs_dir_leaf *)leaf_bh->b_data;
	count = le32_to_cpu(leaf->dl_count);

	for (i = 0; i < count; i++) {
		entry_size = sizeof(struct aetherfs_dir_hash_entry) +
			le16_to_cpu(leaf->dl_entries[i].dhe_namelen);
		entry = (struct aetherfs_dir_hash_entry *)((char *)leaf->dl_entries + i * 32);

		if (le32_to_cpu(entry->dhe_hash) == hash &&
		    le16_to_cpu(entry->dhe_namelen) == namelen &&
		    memcmp(entry->dhe_name, name, namelen) == 0) {
			*ino = le64_to_cpu(entry->dhe_ino);
			ret = 0;
			break;
		}
	}

	brelse(leaf_bh);
	return ret;
}

int aetherfs_dir_htree_delete(struct inode *dir, const char *name, size_t namelen)
{
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh;
	struct aetherfs_dir_htree *htree;
	uint64_t leaf_block;
	int ret;

	ret = aetherfs_dir_htree_lookup(dir, name, namelen, &leaf_block);
	if (ret)
		return ret;

	bh = sb_bread(sb, dir->i_ino);
	if (!bh)
		return -EIO;

	htree = (struct aetherfs_dir_htree *)bh->b_data;
	htree->h_entries = cpu_to_le32(le32_to_cpu(htree->h_entries) - 1);
	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int aetherfs_dir_ordered_index_insert(struct inode *dir, const char *name,
				       size_t namelen, uint64_t ino)
{
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh, *idx_bh;
	struct aetherfs_dir_htree *htree;
	struct aetherfs_dir_ordered_index *ordered;
	struct aetherfs_dir_hash_entry *entry;
	size_t entry_size;
	uint64_t idx_block;
	uint32_t count, i;

	bh = sb_bread(sb, dir->i_ino);
	if (!bh)
		return -EIO;

	htree = (struct aetherfs_dir_htree *)bh->b_data;
	idx_block = le64_to_cpu(htree->h_ordered_root);

	if (idx_block == 0) {
		idx_block = aetherfs_alloc_blocks(dir, 0, 1);
		if (!idx_block) {
			brelse(bh);
			return -ENOSPC;
		}
		htree->h_ordered_root = cpu_to_le64(idx_block);
		set_buffer_dirty(bh);
	}

	brelse(bh);

	idx_bh = sb_getblk(sb, idx_block);
	if (!idx_bh)
		return -EIO;

	ordered = (struct aetherfs_dir_ordered_index *)idx_bh->b_data;

	if (le32_to_cpu(ordered->oi_type) == 0) {
		ordered->oi_type = cpu_to_le32(DIR_INDEX_ORDERED);
		ordered->oi_count = cpu_to_le32(0);
		ordered->oi_next = cpu_to_le64(0);
	}

	entry_size = sizeof(struct aetherfs_dir_hash_entry) + namelen;
	entry = (struct aetherfs_dir_hash_entry *)((char *)ordered->oi_entries +
		le32_to_cpu(ordered->oi_count) * entry_size);

	entry->dhe_ino = cpu_to_le64(ino);
	entry->dhe_namelen = cpu_to_le16(namelen);
	entry->dhe_flags = cpu_to_le16(0);
	memcpy(entry->dhe_name, name, namelen);

	ordered->oi_count = cpu_to_le32(le32_to_cpu(ordered->oi_count) + 1);
	ordered->oi_checksum = cpu_to_le32(
		aetherfs_crc32(ordered, sb->s_blocksize - 4));

	set_buffer_dirty(idx_bh);
	sync_dirty_buffer(idx_bh);
	brelse(idx_bh);

	return 0;
}

int aetherfs_dir_iterate(struct file *filp, struct dir_context *ctx)
{
	struct inode *dir = file_inode(filp);
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh, *leaf_bh;
	struct aetherfs_dir_htree *htree;
	struct aetherfs_dir_leaf *leaf;
	struct aetherfs_dir_hash_entry *entry;
	uint64_t leaf_block;
	uint32_t count, i;
	size_t entry_size;
	int ret = 0;

	if (!dir_emit_dots(filp, ctx))
		return 0;

	bh = sb_bread(sb, dir->i_ino);
	if (!bh)
		return -EIO;

	htree = (struct aetherfs_dir_htree *)bh->b_data;
	leaf_block = le64_to_cpu(htree->h_root_block);
	brelse(bh);

	if (leaf_block == 0)
		return 0;

	leaf_bh = sb_getblk(sb, leaf_block);
	if (!leaf_bh)
		return -EIO;

	leaf = (struct aetherfs_dir_leaf *)leaf_bh->b_data;
	count = le32_to_cpu(leaf->dl_count);

	for (i = 0; i < count && ctx->pos < count; i++) {
		entry_size = sizeof(struct aetherfs_dir_hash_entry) +
			le16_to_cpu(leaf->dl_entries[i].dhe_namelen);
		entry = &leaf->dl_entries[i];

		if (!dir_emit(ctx, entry->dhe_name, le16_to_cpu(entry->dhe_namelen),
			      le64_to_cpu(entry->dhe_ino), DT_UNKNOWN))
			break;

		ctx->pos++;
	}

	brelse(leaf_bh);
	return ret;
}

int aetherfs_dir_split_if_needed(struct inode *dir)
{
	struct super_block *sb = dir->i_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct buffer_head *bh;
	struct aetherfs_dir_htree *htree;
	uint32_t entries;

	bh = sb_bread(sb, dir->i_ino);
	if (!bh)
		return -EIO;

	htree = (struct aetherfs_dir_htree *)bh->b_data;
	entries = le32_to_cpu(htree->h_entries);

	if (entries >= AETHERFS_DIR_LEAF_ENTRIES * 4) {
		uint32_t ag = (dir->i_ino >> 20) % info->s_groups;
		uint64_t new_block = aetherfs_alloc_blocks(dir, ag * AETHERFS_BLOCKS_PER_GROUP, 1);

		if (new_block) {
			htree->h_depth = cpu_to_le32(1);
			set_buffer_dirty(bh);
			pr_info("AetherFS: dir ino %lu split to depth %u\n",
				dir->i_ino, 1);
		}
	}

	brelse(bh);
	return 0;
}