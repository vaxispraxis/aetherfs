#include "aetherfs.h"

int aetherfs_write_inline(struct inode *inode, const void *data, uint64_t size)
{
	struct aetherfs_inode_info *ei;
	struct aetherfs_inode *raw;
	struct buffer_head *bh;
	uint32_t flags;
	int err;

	if (!inode || !data)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);
	flags = le32_to_cpu(ei->i_flags);

	if (size > AETHERFS_MAX_INLINE - 100)
		return -ENOSPC;

	bh = sb_bread(inode->i_sb, inode->i_ino);
	if (!bh)
		return -EIO;

	raw = (struct aetherfs_inode *)bh->b_data;
	raw->i_inline_len = cpu_to_le32((uint32_t)size);
	raw->i_flags = cpu_to_le32(le32_to_cpu(raw->i_flags) | AETHERFS_F_INLINE);

	memcpy(raw->i_inline, data, size);

	raw->i_checksum = cpu_to_le32(aetherfs_checksum_metadata(raw));

	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	i_size_write(inode, size);
	mark_inode_dirty(inode);

	return 0;
}

int aetherfs_read_inline(struct inode *inode, void *data, uint64_t size)
{
	struct aetherfs_inode_info *ei;
	struct aetherfs_inode *raw;
	struct buffer_head *bh;
	uint32_t inline_len;
	uint64_t inode_size;
	int err;

	if (!inode || !data)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);
	inode_size = i_size_read(inode);

	if (!(le32_to_cpu(ei->i_flags) & AETHERFS_F_INLINE))
		return -EINVAL;

	bh = sb_bread(inode->i_sb, inode->i_ino);
	if (!bh)
		return -EIO;

	raw = (struct aetherfs_inode *)bh->b_data;
	inline_len = le32_to_cpu(raw->i_inline_len);

	if (inline_len > AETHERFS_MAX_INLINE - 100) {
		brelse(bh);
		return -EIO;
	}

	if (size < inline_len)
		size = inline_len;

	memcpy(data, raw->i_inline, size);

	brelse(bh);

	return (int)size;
}

int aetherfs_convert_inline(struct inode *inode)
{
	struct aetherfs_inode_info *ei;
	struct aetherfs_inode *raw;
	struct buffer_head *bh, *data_bh;
	void *inline_data;
	uint64_t size;
	unsigned long block;
	int err;

	if (!inode)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);
	size = i_size_read(inode);

	if (!(le32_to_cpu(ei->i_flags) & AETHERFS_F_INLINE))
		return 0;

	if (size > AETHERFS_MAX_INLINE - 100)
		return -ENOSPC;

	bh = sb_bread(inode->i_sb, inode->i_ino);
	if (!bh)
		return -EIO;

	raw = (struct aetherfs_inode *)bh->b_data;
	inline_data = raw->i_inline;

	block = aetherfs_alloc_blocks(inode, 0, 1);
	if (!block) {
		brelse(bh);
		return -ENOSPC;
	}

	data_bh = sb_getblk(inode->i_sb, block);
	if (!data_bh) {
		brelse(bh);
		return -EIO;
	}

	memcpy(data_bh->b_data, inline_data, le32_to_cpu(raw->i_inline_len));

	set_buffer_dirty(data_bh);
	sync_dirty_buffer(data_bh);
	brelse(data_bh);

	raw->i_flags = cpu_to_le32(le32_to_cpu(raw->i_flags) & ~AETHERFS_F_INLINE);
	raw->i_extent_root = cpu_to_le64(block);

	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

int aetherfs_should_inline(struct inode *inode, uint64_t size)
{
	struct aetherfs_inode_info *ei;

	if (!inode)
		return 0;

	ei = AETHERFS_INODE(inode);

	if (S_ISDIR(inode->i_mode))
		return 1;

	if (S_ISLNK(inode->i_mode) && size < AETHERFS_MAX_SYMLINK)
		return 1;

	if (S_ISREG(inode->i_mode) && size <= AETHERFS_MAX_INLINE)
		return 1;

	return 0;
}

int aetherfs_get_block_inline(struct inode *inode, unsigned long iblock,
			struct buffer_head *bh_result, int create)
{
	if (!inode || !bh_result)
		return -EINVAL;

	bh_result->b_size = i_size_read(inode);
	map_bh(bh_result, inode->i_sb, 0);

	return 0;
}