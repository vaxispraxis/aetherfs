#include "aetherfs.h"

static int aetherfs_snapshot_create(struct inode *dir, struct dentry *dentry,
				int readonly)
{
	struct super_block *sb = dir->i_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct inode *snap_inode;
	struct aetherfs_snapshot_ref *snap;
	struct buffer_head *bh;
	uint64_t snap_id;
	int err;

	if (!dir || !dentry)
		return -EINVAL;

	snap_id = ++info->s_generation;

	snap_inode = aetherfs_new_inode(dir, S_IFDIR | 0755, 0);
	if (IS_ERR(snap_inode))
		return PTR_ERR(snap_inode);

	snap_inode->i_op = &aetherfs_dir_inops;
	snap_inode->i_fop = &aetherfs_dir_fops;

	if (readonly)
		aetherfs_inode_info(snap_inode)->i_flags |= AETHERFS_F_IMMUTABLE;

	bh = sb_bread(sb, info->s_es->s_checkpoint_root);
	if (!bh) {
		iput(snap_inode);
		return -EIO;
	}

	snap = (struct aetherfs_snapshot_ref *)bh->b_data;
	snap->sr_id = cpu_to_le64(snap_id);
	snap->sr_gen_time = cpu_to_le64(current_time(snap_inode).tv_sec);
	snap->sr_root_inode = cpu_to_le64(info->s_es->s_root_inode);
	snap->sr_extent_tree = cpu_to_le64(info->s_es->s_extent_tree);
	snap->sr_meta_tree = cpu_to_le64(info->s_es->s_meta_tree);
	snap->sr_refcount = cpu_to_le32(1);
	snap->sr_flags = cpu_to_le32(readonly ? AETHERFS_SNAP_READONLY : 0);

	set_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	d_instantiate(dentry, snap_inode);
	unlock_new_inode(snap_inode);

	return 0;
}

int aetherfs_snapshot_ro(struct inode *inode)
{
	struct aetherfs_inode_info *ei;

	if (!inode)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);
	ei->i_flags |= cpu_to_le32(AETHERFS_F_IMMUTABLE);

	mark_inode_dirty(inode);
	return 0;
}

int aetherfs_snapshot_rw(struct inode *inode)
{
	struct aetherfs_inode_info *ei;

	if (!inode)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);
	ei->i_flags &= cpu_to_le32(~AETHERFS_F_IMMUTABLE);

	mark_inode_dirty(inode);
	return 0;
}

int aetherfs_snapshot_delete(struct inode *inode)
{
	struct aetherfs_inode_info *ei;
	uint32_t refcount;

	if (!inode)
		return -EINVAL;

	ei = AETHERFS_INODE(inode);
	refcount = le32_to_cpu(ei->i_refcount);

	if (refcount > 1) {
		refcount--;
		ei->i_refcount = cpu_to_le32(refcount);
		return 0;
	}

(drop_ref):
	atomic_dec(&inode->i_count);
	iput(inode);

	return 0;
}

int aetherfs_snapshot_list(struct super_block *sb, void *arg,
		       void (*callback)(void *arg, struct aetherfs_snapshot_ref *snap))
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_snapshot_ref *snap;
	struct buffer_head *bh;
	uint64_t root;
	int i;

	if (!sb || !callback)
		return -EINVAL;

	root = le64_to_cpu(info->s_es->s_checkpoint_root);

	for (i = 0; i < 8; i++) {
		bh = sb_bread(sb, root + i);
		if (!bh)
			continue;

		snap = (struct aetherfs_snapshot_ref *)bh->b_data;
		if (snap->sr_id)
			callback(arg, snap);

		brelse(bh);
	}

	return 0;
}

int aetherfs_snapshot_rollback(struct super_block *sb, uint64_t snap_id)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_snapshot_ref *snap;
	struct buffer_head *bh;
	uint64_t root;
	int i;

	if (!sb)
		return -EINVAL;

	root = le64_to_cpu(info->s_es->s_checkpoint_root);

	for (i = 0; i < 8; i++) {
		bh = sb_bread(sb, root + i);
		if (!bh)
			continue;

		snap = (struct aetherfs_snapshot_ref *)bh->b_data;
		if (le64_to_cpu(snap->sr_id) == snap_id) {
			info->s_es->s_root_inode = snap->sr_root_inode;
			info->s_es->s_extent_tree = snap->sr_extent_tree;
			info->s_es->s_meta_tree = snap->sr_meta_tree;

			set_buffer_dirty(info->s_sbh);
			sync_dirty_buffer(info->s_sbh);

			brelse(bh);
			return 0;
		}

		brelse(bh);
	}

	return -ENOENT;
}