#include "aetherfs.h"
#include <linux/spinlock.h>
#include <linux/bitmap.h>

#define AETHERFS_FREE_TREE_ORDER    32
#define AETHERFS_BITMAP_BITS       (4096 * 8)
#define AETHERFS_AG_BITMAP_BLOCKS  1

enum aetherfs_allocation_class {
	AETHERFS_ALLOC_HOT   = 0,
	AETHERFS_ALLOC_WARM  = 1,
	AETHERFS_ALLOC_COLD  = 2,
	AETHERFS_ALLOC_TEMP_COUNT = 3
};

struct aetherfs_free_tree {
	struct aetherfs_btree_node *ft_root;
	rwlock_t ft_lock;
	atomic_t ft_refs;
};

struct aetherfs_ag_bitmap {
	unsigned long *ab_bitmap;
	uint32_t ab_bits;
	uint32_t ab_free;
	uint32_t ab_reserved;
 spinlock_t ab_lock;
};

struct aetherfs_alloc_class {
	enum aetherfs_allocation_class ac_class;
	uint32_t ac_preferred_ag;
	uint32_t ac_min_size;
	uint32_t ac_max_size;
	uint8_t ac_device_hint;
};

struct aetherfs_free_space_info {
	struct aetherfs_free_tree fs_free_tree;
	struct aetherfs_ag_bitmap *fs_ag_bitmaps;
	uint32_t fs_ag_count;
	struct aetherfs_alloc_class fs_classes[AETHERFS_ALLOC_TEMP_COUNT];
 spinlock_t fs_lock;
};

static DEFINE_SPINLOCK(free_space_lock);
static struct kmem_cache *free_extent_cache;
static struct kmem_cache *ag_bitmap_cache;

static inline uint32_t find_next_zero_bit_wrap(unsigned long *bitmap, 
						uint32_t size, uint32_t offset)
{
	uint32_t pos = find_next_zero_bit(bitmap, size, offset);
	if (pos >= size)
		pos = find_first_zero_bit(bitmap, size);
	return pos;
}

static int free_tree_insert_extent(struct aetherfs_free_tree *tree,
				    uint64_t start, uint64_t blocks)
{
	struct aetherfs_free_extent *extent;
	int ret;

	if (!tree || !start || !blocks)
		return -EINVAL;

	extent = kmem_cache_zalloc(free_extent_cache, GFP_NOFS);
	if (!extent)
		return -ENOMEM;

	extent->fe_pstart = cpu_to_le64(start);
	extent->fe_len = cpu_to_le64(blocks);
	extent->fe_next = cpu_to_le64(0);
	extent->fe_checksum = cpu_to_le32(
		aetherfs_crc32(extent, sizeof(*extent) - 4));

	ret = aetherfs_btree_insert_extent(&tree->ft_root, 
					     0, start, start, blocks);

	kmem_cache_free(free_extent_cache, extent);
	return ret;
}

static int free_tree_remove_extent(struct aetherfs_free_tree *tree,
				    uint64_t start, uint64_t blocks)
{
	if (!tree || !start)
		return -EINVAL;

	return aetherfs_btree_delete_extent(&tree->ft_root, 0, start);
}

static int free_tree_find_contiguous(struct aetherfs_free_tree *tree,
				      uint64_t wanted, uint64_t *found_start)
{
	uint64_t pblock = 0;
	uint32_t blocks = 0;
	int ret;

	if (!tree || !found_start)
		return -EINVAL;

	ret = aetherfs_btree_lookup_extent(tree->ft_root, 0, 0, &pblock, &blocks);
	if (ret)
		return ret;

	if (blocks >= wanted) {
		*found_start = pblock;
		return 0;
	}

	return -ENOSPC;
}

static int ag_bitmap_init(struct aetherfs_ag_bitmap *agb, uint32_t bits)
{
	if (!agb || !bits)
		return -EINVAL;

	agb->ab_bitmap = kmalloc(BITS_TO_LONGS(bits) * sizeof(unsigned long), 
				  GFP_NOFS);
	if (!agb->ab_bitmap)
		return -ENOMEM;

	bitmap_zero(agb->ab_bitmap, bits);
	agb->ab_bits = bits;
	agb->ab_free = bits;
	agb->ab_reserved = 0;
 spin_lock_init(&agb->ab_lock);

	return 0;
}

static void ag_bitmap_destroy(struct aetherfs_ag_bitmap *agb)
{
	if (!agb)
		return;
	kfree(agb->ab_bitmap);
	agb->ab_bitmap = NULL;
}

static int ag_bitmap_alloc(struct aetherfs_ag_bitmap *agb, uint32_t count,
			   uint64_t *block)
{
	uint32_t pos;
	int ret = 0;

	if (!agb || !block || !count)
		return -EINVAL;

 spin_lock(&agb->ab_lock);

	if (agb->ab_free < count) {
		ret = -ENOSPC;
		goto out;
	}

	pos = find_next_zero_bit_wrap(agb->ab_bitmap, agb->ab_bits, 0);
	if (pos >= agb->ab_bits) {
		ret = -ENOSPC;
		goto out;
	}

	bitmap_set(agb->ab_bitmap, pos, count);
	agb->ab_free -= count;
	*block = pos;

out:
 spin_unlock(&agb->ab_lock);
	return ret;
}

static void ag_bitmap_free(struct aetherfs_ag_bitmap *agb, uint64_t block,
			   uint32_t count)
{
	if (!agb || !block || !count)
		return;

 spin_lock(&agb->ab_lock);
	bitmap_clear(agb->ab_bitmap, block, count);
	agb->ab_free += count;
 spin_unlock(&agb->ab_lock);
}

static int free_space_init_class(struct aetherfs_alloc_class *ac,
				  enum aetherfs_allocation_class class)
{
	if (!ac)
		return -EINVAL;

	ac->ac_class = class;
	ac->ac_preferred_ag = 0;
	ac->ac_min_size = 1;
	ac->ac_max_size = 256;
	ac->ac_device_hint = 0;

	switch (class) {
	case AETHERFS_ALLOC_HOT:
		ac->ac_min_size = 1;
		ac->ac_max_size = 16;
		break;
	case AETHERFS_ALLOC_WARM:
		ac->ac_min_size = 16;
		ac->ac_max_size = 128;
		break;
	case AETHERFS_ALLOC_COLD:
		ac->ac_min_size = 128;
		ac->ac_max_size = 1024;
		break;
	}

	return 0;
}

int aetherfs_free_space_init(struct super_block *sb, uint32_t ag_count)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_free_space_info *fsi;
	uint32_t i;
	int ret;

	if (!sb || !info)
		return -EINVAL;

	fsi = kzalloc(sizeof(struct aetherfs_free_space_info), GFP_NOFS);
	if (!fsi)
		return -ENOMEM;

	fsi->fs_ag_count = ag_count;
	fsi->fs_ag_bitmaps = kcalloc(ag_count, sizeof(struct aetherfs_ag_bitmap),
				     GFP_NOFS);
	if (!fsi->fs_ag_bitmaps) {
		kfree(fsi);
		return -ENOMEM;
	}

	for (i = 0; i < ag_count; i++) {
		ret = ag_bitmap_init(&fsi->fs_ag_bitmaps[i], AETHERFS_BITMAP_BITS);
		if (ret) {
			while (i > 0) {
				i--;
				ag_bitmap_destroy(&fsi->fs_ag_bitmaps[i]);
			}
			kfree(fsi->fs_ag_bitmaps);
			kfree(fsi);
			return ret;
		}
	}

	for (i = 0; i < AETHERFS_ALLOC_TEMP_COUNT; i++) {
		free_space_init_class(&fsi->fs_classes[i], i);
	}

 spin_lock_init(&fsi->fs_lock);

	info->s_private = fsi;

	pr_info("AetherFS: free space initialized with %u allocation groups\n", ag_count);
	return 0;
}

void aetherfs_free_space_destroy(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_free_space_info *fsi;
	uint32_t i;

	if (!sb || !info)
		return;

	fsi = info->s_private;
	if (!fsi)
		return;

	for (i = 0; i < fsi->fs_ag_count; i++) {
		ag_bitmap_destroy(&fsi->fs_ag_bitmaps[i]);
	}

	kfree(fsi->fs_ag_bitmaps);
	kfree(fsi);
	info->s_private = NULL;
}

uint64_t aetherfs_alloc_blocks_extended(struct super_block *sb,
					 uint32_t count,
					 enum aetherfs_allocation_class class,
					 uint32_t ag_hint)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_free_space_info *fsi;
	struct aetherfs_alloc_class *ac;
	uint64_t block = 0;
	uint32_t ag;
	int ret;

	if (!sb || !info || !count)
		return 0;

	fsi = info->s_private;
	if (!fsi)
		return 0;

	ac = &fsi->fs_classes[class];

	if (ag_hint >= fsi->fs_ag_count)
		ag_hint = 0;

	for (ag = 0; ag < fsi->fs_ag_count; ag++) {
		uint32_t try_ag = (ag_hint + ag) % fsi->fs_ag_count;
		ret = ag_bitmap_alloc(&fsi->fs_ag_bitmaps[try_ag], count, &block);
		if (ret == 0) {
			block += try_ag * AETHERFS_BLOCKS_PER_GROUP;
			block += AETHERFS_DATA_START_BLOCK;
			info->s_free_blocks -= count;
			return block;
		}
	}

	return 0;
}

void aetherfs_free_blocks_extended(struct super_block *sb, 
				     uint64_t start, uint32_t count)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_free_space_info *fsi;
	uint64_t ag_offset;
	uint32_t ag;
	uint64_t block_in_ag;

	if (!sb || !info || !start || !count)
		return;

	fsi = info->s_private;
	if (!fsi)
		return;

	ag_offset = start - AETHERFS_DATA_START_BLOCK;
	if (ag_offset >= fsi->fs_ag_count * AETHERFS_BLOCKS_PER_GROUP)
		return;

	ag = ag_offset / AETHERFS_BLOCKS_PER_GROUP;
	block_in_ag = ag_offset % AETHERFS_BLOCKS_PER_GROUP;

	if (ag < fsi->fs_ag_count) {
		ag_bitmap_free(&fsi->fs_ag_bitmaps[ag], block_in_ag, count);
		info->s_free_blocks += count;
	}
}

int aetherfs_free_space_init_module(void)
{
	free_extent_cache = kmem_cache_create("aetherfs_free_extent",
						sizeof(struct aetherfs_free_extent),
						0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!free_extent_cache)
		return -ENOMEM;

	ag_bitmap_cache = kmem_cache_create("aetherfs_ag_bitmap",
					     sizeof(struct aetherfs_ag_bitmap),
					     0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!ag_bitmap_cache) {
		kmem_cache_destroy(free_extent_cache);
		return -ENOMEM;
	}

	pr_info("AetherFS: free space management module initialized\n");
	return 0;
}

void aetherfs_free_space_exit_module(void)
{
	if (free_extent_cache)
		kmem_cache_destroy(free_extent_cache);
	if (ag_bitmap_cache)
		kmem_cache_destroy(ag_bitmap_cache);
}

unsigned long aetherfs_alloc_blocks(struct inode *inode, unsigned long start, unsigned long len)
{
	struct super_block *sb = inode->i_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	unsigned long block;

	if (!info || !info->s_free_blocks)
		return 0;

	block = (info->s_next_ino++ % AETHERFS_BLOCKS_PER_GROUP) + AETHERFS_DATA_START_BLOCK;
	if (block >= le64_to_cpu(info->s_es->s_blocks_count))
		block = AETHERFS_DATA_START_BLOCK;

	info->s_free_blocks -= len;
	info->s_dirty = 1;

	return block;
}

void aetherfs_free_blocks(struct super_block *sb, unsigned long start, unsigned long len)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);

	if (!info || !start || !len)
		return;

	info->s_free_blocks += len;
	info->s_dirty = 1;
}