#include "aetherfs.h"

int aetherfs_cow_inode(struct inode *inode)
{
	if (!inode)
		return -EINVAL;
	return 0;
}

int aetherfs_cow_extent(struct inode *inode, unsigned long lblk, unsigned long len)
{
	if (!inode || !len)
		return -EINVAL;
	return 0;
}

int aetherfs_write_with_checksum(struct buffer_head *bh, void *data, size_t len)
{
	if (!bh || !data || !len)
		return -EINVAL;
	memcpy(bh->b_data, data, len);
	set_buffer_dirty(bh);
	return 0;
}

int aetherfs_read_verify(struct buffer_head *bh, void *data, size_t len)
{
	if (!bh || !data || !len)
		return -EINVAL;
	memcpy(data, bh->b_data, len);
	return 0;
}

int aetherfs_device_init(struct super_block *sb, struct aetherfs_device_group *dg)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	if (!sb || !dg)
		return -EINVAL;
	info->s_dev_group = dg;
	info->s_dev_count = le32_to_cpu(dg->dg_count);
	info->s_alloc = kzalloc(sizeof(struct aetherfs_allocator), GFP_KERNEL);
	if (!info->s_alloc)
		return -ENOMEM;
	info->s_alloc->a_preferred_dev = 0;
	info->s_alloc->a_max_segments = 256;
	info->s_alloc->a_queue_depth = 32;
	return 0;
}

int aetherfs_device_failed(struct super_block *sb, int devnr)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_device_group *dg;
	if (!sb || !info)
		return -EINVAL;
	dg = info->s_dev_group;
	if (!dg || devnr < 0 || devnr >= le32_to_cpu(dg->dg_count))
		return -EINVAL;
	if (devnr == 0)
		return -EIO;
	dg->dg_devs[devnr].d_flags |= AETHERFS_DEV_FAILED;
	return 0;
}

int aetherfs_device_online(struct super_block *sb, int devnr)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_device_group *dg;
	if (!sb || !info)
		return -EINVAL;
	dg = info->s_dev_group;
	if (!dg || devnr < 0 || devnr >= le32_to_cpu(dg->dg_count))
		return -EINVAL;
	dg->dg_devs[devnr].d_flags &= ~AETHERFS_DEV_FAILED;
	return 0;
}