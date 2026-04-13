#include "aetherfs.h"

static int aetherfs_extent_set(struct inode *inode, unsigned long lblock,
                              unsigned long pblock, unsigned int len)
{
	struct aetherfs_extent_node *en;
	int err = 0;

	if (!inode || !lblock || !len)
		return -EINVAL;

	en = kzalloc(sizeof(*en), GFP_NOFS);
	if (!en)
		return -ENOMEM;
	en->en_count = 1;
	en->en_extents[0].e_lstart = cpu_to_le64(lblock);
	en->en_extents[0].e_pstart = cpu_to_le64(pblock);
	en->en_extents[0].e_len = cpu_to_le32(len);

	return 0;
}

static int aetherfs_extent_get(struct inode *inode, unsigned long lblock,
                             unsigned long *pblock, unsigned int *len)
{
	if (!inode || !pblock || !len)
		return -EINVAL;
	return -ENOENT;
}

uint64_t aetherfs_bmap(struct inode *inode, uint64_t lblock)
{
	unsigned long pblock = 0;
	unsigned int len = 0;

	if (!inode)
		return 0;

	aetherfs_extent_get(inode, lblock, &pblock, &len);
	return pblock;
}

int aetherfs_get_blocks(struct inode *inode, sector_t iblock,
                      unsigned int max_blocks, struct buffer_head *bh, int create)
{
	struct super_block *sb = inode->i_sb;
	unsigned long pblock = 0;
	unsigned int len = 0;
	int err = 0;

	if (!inode || !bh)
		return -EINVAL;

	err = aetherfs_extent_get(inode, iblock, &pblock, &len);

	if (err && !create)
		return err;

	if (create) {
		pblock = aetherfs_alloc_blocks(inode, iblock, 1);
		if (!pblock)
			return -ENOSPC;

		err = aetherfs_extent_set(inode, iblock, pblock, max_blocks);
		if (err)
			return err;

		set_buffer_new(bh);
	}

	map_bh(bh, sb, pblock);
	bh->b_size = len * sb->s_blocksize;
	return 0;
}