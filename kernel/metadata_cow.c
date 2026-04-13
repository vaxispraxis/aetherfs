#include "aetherfs.h"

int aetherfs_cow_inode_metadata(struct inode *inode)
{
	return 0;
}

int aetherfs_cow_extent_tree(struct inode *inode, uint64_t tree_root_old, uint64_t *tree_root_new)
{
	if (!tree_root_new)
		return -EINVAL;
	*tree_root_new = tree_root_old;
	return 0;
}

int aetherfs_cow_dir_block(struct inode *dir, uint64_t block_old, uint64_t *block_new)
{
	if (!block_new)
		return -EINVAL;
	*block_new = block_old;
	return 0;
}

int aetherfs_cow_alloc_map(struct super_block *sb, uint64_t bitmap_block_old, uint64_t *bitmap_block_new)
{
	if (!bitmap_block_new)
		return -EINVAL;
	*bitmap_block_new = bitmap_block_old;
	return 0;
}

int aetherfs_cow_superblock(struct super_block *sb, struct aetherfs_super *old_es, struct aetherfs_super **new_es)
{
	if (!new_es)
		return -EINVAL;
	*new_es = old_es;
	return 0;
}

int aetherfs_cow_metadata_update(struct super_block *sb, enum aetherfs_meta_type type,
				  uint64_t old_block, uint64_t *new_block)
{
	if (!sb || !new_block)
		return -EINVAL;

	*new_block = old_block;
	return 0;
}