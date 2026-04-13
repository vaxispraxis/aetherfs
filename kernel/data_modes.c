#include "aetherfs.h"

int aetherfs_set_data_mode(struct inode *inode, int mode)
{
	struct aetherfs_inode_info *ei;

	if (!inode)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);

	switch (mode) {
	case AETHERFS_COW_MODE:
		ei->i_flags &= ~AETHERFS_F_APPEND;
		ei->i_flags |= AETHERFS_F_COW;
		break;
	case AETHERFS_OVERWRITE_MODE:
		ei->i_flags &= ~(AETHERFS_F_COW | AETHERFS_F_APPEND);
		break;
	case AETHERFS_APPEND_MODE:
		ei->i_flags &= ~AETHERFS_F_COW;
		ei->i_flags |= AETHERFS_F_APPEND;
		break;
	default:
		return -EINVAL;
	}

	mark_inode_dirty(inode);
	return 0;
}

int aetherfs_get_data_mode(struct inode *inode)
{
	struct aetherfs_inode_info *ei;
	u32 flags;

	if (!inode)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);
	flags = le32_to_cpu(ei->i_flags);

	if (flags & AETHERFS_F_APPEND)
		return AETHERFS_APPEND_MODE;
	if (flags & AETHERFS_F_COW)
		return AETHERFS_COW_MODE;
	return AETHERFS_OVERWRITE_MODE;
}

static int aetherfs_mode_overwrite_write(struct inode *inode, sector_t iblock,
				   char *buf, unsigned int len)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;

	bh = sb_bread(sb, iblock);
	if (!bh)
		return -EIO;

	memcpy(bh->b_data, buf, len);
	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int aetherfs_write_mode(struct inode *inode, sector_t iblock, char *buf, unsigned int len)
{
	int mode;
	unsigned int blocks;

	if (!inode || !buf)
		return -EINVAL;

	mode = aetherfs_get_data_mode(inode);
	blocks = (len + inode->i_sb->s_blocksize - 1) >> inode->i_sb->s_blocksize_bits;

	if (mode == AETHERFS_COW_MODE && blocks <= AETHERFS_COW_THRESHOLD) {
		struct super_block *sb = inode->i_sb;
		struct buffer_head *bh, *new_bh;
		sector_t new_block;

		bh = sb_bread(sb, iblock);
		if (!bh)
			return -EIO;

		new_block = aetherfs_alloc_blocks(inode, iblock, 1);
		if (!new_block) {
			brelse(bh);
			return -ENOSPC;
		}

		new_bh = sb_getblk(sb, new_block);
		if (!new_bh) {
			brelse(bh);
			return -EIO;
		}

		memcpy(new_bh->b_data, bh->b_data, sb->s_blocksize);
		memcpy(new_bh->b_data, buf, len);
		set_buffer_dirty(new_bh);
		sync_dirty_buffer(new_bh);

		brelse(bh);
		brelse(new_bh);

		return 0;
	}

	return aetherfs_mode_overwrite_write(inode, iblock, buf, len);
}

int aetherfs_should_cow(struct inode *inode)
{
	return aetherfs_get_data_mode(inode) == AETHERFS_COW_MODE;
}

int aetherfs_should_append(struct inode *inode)
{
	return aetherfs_get_data_mode(inode) == AETHERFS_APPEND_MODE;
}