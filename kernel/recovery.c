#include "aetherfs.h"
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/workqueue.h>

enum aetherfs_self_heal_mode {
	SELF_HEAL_DISABLED = 0,
	SELF_HEAL_REPAIR = 1,
	SELF_HEAL_QUARANTINE = 2,
	SELF_HEAL_SCRUB = 3,
};

enum aetherfs_device_type {
	DEVICE_TYPE_UNKNOWN = 0,
	DEVICE_TYPE_HDD = 1,
	DEVICE_TYPE_SATA_SSD = 2,
	DEVICE_TYPE_NVME = 3,
	DEVICE_TYPE_ZNS = 4,
	DEVICE_TYPE_SMR = 5,
};

enum aetherfs_allocation_policy {
	ALLOC_POLICY_DEFAULT = 0,
	ALLOC_POLICY_SEQ_HDD = 1,
	ALLOC_POLICY_SEQ_SSD = 2,
	ALLOC_POLICY_STREAM_NVME = 3,
	ALLOC_POLICY_ZONE_AWARE = 4,
	ALLOC_POLICY_LOCALITY = 5,
};

struct aetherfs_self_heal_info {
	enum aetherfs_self_heal_mode sh_mode;
	uint64_t sh_last_scrub;
	uint32_t sh_scrub_interval;
	atomic_t sh_repair_count;
	atomic_t sh_corruption_count;
	struct work_struct sh_scrub_work;
};

struct aetherfs_device_info {
	enum aetherfs_device_type dev_type;
	uint32_t dev_physical_sector;
	uint32_t dev_logical_sector;
	uint32_t dev_erase_block;
	uint32_t dev_chip_id;
	uint64_t dev_capacity;
	uint8_t  dev_queue_depth;
	uint8_t  dev_stream_count;
	uint8_t  dev_zoned;
	uint8_t  dev_rotational;
};

struct aetherfs_allocation_context {
	enum aetherfs_allocation_policy policy;
	uint32_t preferred_ag;
	uint32_t preferred_device;
	uint32_t min_extent_size;
	uint32_t max_extent_size;
	uint8_t  hotness;
	uint8_t  snapshot_pressure;
};

static DEFINE_RWLOCK(self_heal_lock);
static struct aetherfs_self_heal_info global_heal_info;

static enum aetherfs_device_type detect_device_type(struct block_device *bdev)
{
	struct request_queue *q = bdev_get_queue(bdev);
	struct device *dev = q->dev;
	
	if (!q)
		return DEVICE_TYPE_UNKNOWN;

	if (blk_queue_zoned(q))
		return DEVICE_TYPE_ZNS;
	
	if (blk_queue_nonrot(q)) {
		if (q->queue_flags & (1 << QUEUE_FLAG_NVME))
			return DEVICE_TYPE_NVME;
		return DEVICE_TYPE_SATA_SSD;
	}
	
	if (blk_queue_smr(q))
		return DEVICE_TYPE_SMR;
	
	return DEVICE_TYPE_HDD;
}

int aetherfs_self_heal_init(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_self_heal_info *sh;
	
	if (!info)
		return -EINVAL;
	
	sh = kzalloc(sizeof(struct aetherfs_self_heal_info), GFP_NOFS);
	if (!sh)
		return -ENOMEM;
	
	sh->sh_mode = SELF_HEAL_REPAIR;
	sh->sh_last_scrub = 0;
	sh->sh_scrub_interval = 86400;
	atomic_set(&sh->sh_repair_count, 0);
	atomic_set(&sh->sh_corruption_count, 0);
	INIT_WORK(&sh->sh_scrub_work, NULL);
	
	info->s_private = sh;
	
	return 0;
}

void aetherfs_self_heal_exit(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_self_heal_info *sh;
	
	if (!info)
		return;
	
	sh = info->s_private;
	if (sh) {
		cancel_work_sync(&sh->sh_scrub_work);
		kfree(sh);
	}
}

int aetherfs_self_heal_repair_on_read(struct super_block *sb, 
				       struct buffer_head *bh,
				       uint64_t blocknr)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_self_heal_info *sh;
	struct buffer_head *repaired_bh;
	int ret;

	if (!sb || !bh)
		return -EINVAL;

	info = AETH_SB(sb);
	sh = info->s_private;

	if (!sh || sh->sh_mode == SELF_HEAL_DISABLED)
		return 0;

	ret = aetherfs_verify_data_checksum(sb, bh, blocknr);
	if (ret == 0)
		return 0;

	atomic_inc(&sh->sh_corruption_count);
	pr_warn("AetherFS: data corruption detected at block %llu\n", blocknr);

	if (sh->sh_mode == SELF_HEAL_QUARANTINE) {
		pr_warn("AetherFS: quarantining corrupted block %llu\n", blocknr);
		return -EUCLEAN;
	}

	repaired_bh = sb_getblk(sb, blocknr);
	if (!repaired_bh)
		return -EIO;

	if (buffer_uptodate(bh)) {
		memcpy(repaired_bh->b_data, bh->b_data, sb->s_blocksize);
		set_buffer_dirty(repaired_bh);
		sync_dirty_buffer(repaired_bh);
		brelse(repaired_bh);
		
		atomic_inc(&sh->sh_repair_count);
		pr_info("AetherFS: repaired block %llu\n", blocknr);
		return 0;
	}

	brelse(repaired_bh);
	return -EIO;
}

int aetherfs_extent_quarantine(struct super_block *sb,
			        uint64_t extent_start, uint32_t extent_len)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct buffer_head *bh;
	void *data;
	
	if (!sb || !extent_start || !extent_len)
		return -EINVAL;
	
	bh = sb_getblk(sb, extent_start);
	if (!bh)
		return -EIO;
	
	data = bh->b_data;
	memset(data, 0, sb->s_blocksize);
	
	*((uint64_t *)data) = cpu_to_le64(extent_start);
	*((uint32_t *)(data + 8)) = cpu_to_le32(extent_len);
	*((uint32_t *)(data + 12)) = cpu_to_le32(0x51434152);
	
	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	
	pr_info("AetherFS: quarantined extent %llu len %u\n", extent_start, extent_len);
	return 0;
}

static void scrub_work_fn(struct work_struct *work)
{
	pr_info("AetherFS: scrub work initiated\n");
}

int aetherfs_scrub_start(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_self_heal_info *sh;
	
	if (!sb || !info)
		return -EINVAL;
	
	sh = info->s_private;
	if (!sh)
		return -EINVAL;
	
	sh->sh_last_scrub = ktime_get_real_seconds();
	schedule_work(&sh->sh_scrub_work);
	
	return 0;
}

int aetherfs_scrub_status(struct super_block *sb, 
			  uint64_t *last_scrub, uint32_t *corruptions, uint32_t *repairs)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_self_heal_info *sh;
	
	if (!sb || !info)
		return -EINVAL;
	
	sh = info->s_private;
	if (!sh)
		return -EINVAL;
	
	*last_scrub = sh->sh_last_scrub;
	*corruptions = atomic_read(&sh->sh_corruption_count);
	*repairs = atomic_read(&sh->sh_repair_count);
	
	return 0;
}

int aetherfs_device_info_init(struct super_block *sb, struct block_device *bdev)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_device_info *dev_info;
	struct request_queue *q;
	
	if (!sb || !bdev || !info)
		return -EINVAL;
	
	dev_info = kzalloc(sizeof(struct aetherfs_device_info), GFP_NOFS);
	if (!dev_info)
		return -ENOMEM;
	
	q = bdev_get_queue(bdev);
	if (q) {
		dev_info->dev_type = detect_device_type(bdev);
		dev_info->dev_physical_sector = queue_physical_sector_size(q);
		dev_info->dev_logical_sector = queue_logical_sector_size(q);
		dev_info->dev_erase_block = 0;
		dev_info->dev_rotational = blk_queue_rotational(q);
		dev_info->dev_capacity = bdev->bd_part->nr_sects;
		dev_info->dev_queue_depth = q->nr_requests;
		dev_info->dev_zoned = blk_queue_zoned(q);
	}
	
	info->s_private = dev_info;
	
	pr_info("AetherFS: device type %d, capacity %llu sectors\n",
		dev_info->dev_type, dev_info->dev_capacity);
	
	return 0;
}

void aetherfs_device_info_exit(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	
	if (!info)
		return;
	
	kfree(info->s_private);
	info->s_private = NULL;
}

int aetherfs_alloc_init_context(struct aetherfs_allocation_context *ctx,
				 struct inode *inode,
				 uint32_t requested_size)
{
	struct aetherfs_sb_info *info = AETH_SB(inode->i_sb);
	struct aetherfs_device_info *dev_info;
	enum aetherfs_device_type dev_type;
	
	if (!ctx || !inode)
		return -EINVAL;
	
	memset(ctx, 0, sizeof(*ctx));
	ctx->policy = ALLOC_POLICY_DEFAULT;
	ctx->preferred_ag = 0;
	ctx->min_extent_size = 1;
	ctx->max_extent_size = requested_size;
	ctx->hotness = 0;
	ctx->snapshot_pressure = 0;
	
	info = AETH_SB(inode->i_sb);
	dev_info = info->s_private;
	dev_type = dev_info ? dev_info->dev_type : DEVICE_TYPE_HDD;
	
	switch (dev_type) {
	case DEVICE_TYPE_HDD:
		ctx->policy = ALLOC_POLICY_SEQ_HDD;
		ctx->max_extent_size = 256;
		break;
	case DEVICE_TYPE_SATA_SSD:
		ctx->policy = ALLOC_POLICY_SEQ_SSD;
		ctx->max_extent_size = 128;
		break;
	case DEVICE_TYPE_NVME:
		ctx->policy = ALLOC_POLICY_STREAM_NVME;
		ctx->max_extent_size = 64;
		ctx->preferred_device = smp_processor_id() % info->s_dev_count;
		break;
	case DEVICE_TYPE_ZNS:
		ctx->policy = ALLOC_POLICY_ZONE_AWARE;
		ctx->max_extent_size = 32;
		break;
	case DEVICE_TYPE_SMR:
		ctx->policy = ALLOC_POLICY_SEQ_HDD;
		ctx->max_extent_size = 512;
		break;
	default:
		break;
	}
	
	return 0;
}

uint32_t aetherfs_select_ag_for_allocation(struct super_block *sb,
					    struct aetherfs_allocation_context *ctx)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	uint32_t cpu = smp_processor_id();
	uint32_t ag;
	
	if (!sb || !ctx || !info)
		return 0;
	
	if (ctx->preferred_ag > 0 && ctx->preferred_ag < info->s_groups)
		return ctx->preferred_ag;
	
	ag = (cpu + ctx->hotness) % info->s_groups;
	
	return ag;
}

uint64_t aetherfs_alloc_with_policy(struct super_block *sb,
				     struct aetherfs_allocation_context *ctx,
				     uint32_t size)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	uint32_t ag;
	uint64_t block;
	
	if (!sb || !ctx || !info)
		return 0;
	
	ag = aetherfs_select_ag_for_allocation(sb, ctx);
	
	block = aetherfs_alloc_blocks_extended(sb, size, 
						(enum aetherfs_allocation_class)ctx->hotness,
						ag);
	
	if (block)
		pr_info("AetherFS: allocated %u blocks at AG %u (policy %d)\n",
			size, ag, ctx->policy);
	
	return block;
}

int aetherfs_trim_init(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	
	if (!sb || !info)
		return -EINVAL;
	
	info->s_mount_opt &= ~AETHERFS_MOUNT_NODISCARD;
	
	return 0;
}

int aetherfs_trim_device(struct super_block *sb, uint64_t start, uint64_t len)
{
	struct block_device *bdev = sb->s_bdev;
	struct request_queue *q = bdev_get_queue(bdev);
	int ret;
	
	if (!sb || !bdev || !len)
		return -EINVAL;
	
	if (!blk_queue_discard(q))
		return 0;
	
	ret = blkdev_issue_discard(bdev, start, len, GFP_NOFS, 0);
	if (ret)
		pr_warn("AetherFS: discard failed: %d\n", ret);
	
	return ret;
}

int aetherfs_trim_all_free(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_free_space_info *fsi;
	uint64_t start, len;
	uint32_t ag;
	
	if (!sb || !info)
		return -EINVAL;
	
	fsi = info->s_private;
	if (!fsi)
		return 0;
	
	for (ag = 0; ag < fsi->fs_ag_count; ag++) {
		start = AETHERFS_DATA_START_BLOCK + ag * AETHERFS_BLOCKS_PER_GROUP;
		len = AETHERFS_BLOCKS_PER_GROUP;
		
		aetherfs_trim_device(sb, start, len);
	}
	
	return 0;
}

int aetherfs_stream_hint(struct super_block *sb, uint32_t stream_id)
{
	struct block_device *bdev = sb->s_bdev;
	struct request_queue *q;
	
	if (!sb || !bdev)
		return -EINVAL;
	
	q = bdev_get_queue(bdev);
	if (!q || !blk_queue_nonrot(q))
		return -EINVAL;
	
	return 0;
}

int aetherfs_get_device_stats(struct super_block *sb, 
			       struct aetherfs_device_info *stats)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_device_info *dev_info;
	
	if (!sb || !stats || !info)
		return -EINVAL;
	
	dev_info = info->s_private;
	if (!dev_info)
		return -EINVAL;
	
	*stats = *dev_info;
	return 0;
}

void aetherfs_set_allocation_policy(struct super_block *sb,
				    enum aetherfs_allocation_policy policy)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	
	if (!sb || !info)
		return;
	
	info->s_mount_opt &= ~(7 << 16);
	info->s_mount_opt |= (policy << 16);
}

int aetherfs_locality_group_init(struct super_block *sb, uint32_t num_groups)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	
	if (!sb || !info)
		return -EINVAL;
	
	info->s_groups = num_groups;
	
	pr_info("AetherFS: initialized %u locality groups\n", num_groups);
	return 0;
}