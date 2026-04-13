#include "aetherfs.h"

static struct kmem_cache *aetherfs_inode_cache;

struct aetherfs_inode_info {
	struct inode vfs_inode;
	u64 i_extent_root;
	u32 i_flags;
	atomic_t i_count;
};

#define AETHERFS_INODE(i) (container_of(i, struct aetherfs_inode_info, vfs_inode))

static struct inode *aetherfs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	struct aetherfs_inode_info *ei;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct buffer_head *bh;
	struct aetherfs_inode *raw_inode;
	int err;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW))
		return inode;

	ei = AETHERFS_INODE(inode);

	if (ino < AETHERFS_ROOT_INO || ino > le32_to_cpu(info->s_es->s_inodes_count)) {
		err = -EINVAL;
		goto bad_inode;
	}

	bh = sb_bread(sb, ino / AETHERFS_INODES_PER_BLOCK);
	if (!bh) {
		err = -EIO;
		goto bad_inode;
	}

	raw_inode = (struct aetherfs_inode *)bh->b_data + (ino % AETHERFS_INODES_PER_BLOCK);

	inode->i_mode = le16_to_cpu(raw_inode->i_mode);
	i_uid_write(inode, le32_to_cpu(raw_inode->i_uid));
	i_gid_write(inode, le32_to_cpu(raw_inode->i_gid));
	set_nlink(inode, le16_to_cpu(raw_inode->i_links_count));
	i_size_write(inode, le64_to_cpu(raw_inode->i_size));
	inode->i_blocks = le64_to_cpu(raw_inode->i_blocks);
	inode->i_atime.tv_sec = le64_to_cpu(raw_inode->i_atime);
	inode->i_ctime.tv_sec = le64_to_cpu(raw_inode->i_ctime);
	inode->i_mtime.tv_sec = le64_to_cpu(raw_inode->i_mtime);

	ei->i_flags = le32_to_cpu(raw_inode->i_flags);
	ei->i_extent_root = le64_to_cpu(raw_inode->i_extent_root);

	brelse(bh);

	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &aetherfs_file_inops;
		inode->i_fop = &aetherfs_file_operations;
		inode->i_data.a_ops = &aetherfs_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &aetherfs_dir_inops;
		inode->i_fop = &aetherfs_dir_operations;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &aetherfs_symlink_inops;
	} else {
		inode->i_op = &aetherfs_file_inops;
		inode->i_fop = &aetherfs_file_operations;
	}

	unlock_new_inode(inode);
	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(err);
}

struct inode *aetherfs_new_inode(struct inode *dir, umode_t mode, dev_t rdev)
{
	struct super_block *sb = dir->i_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct inode *inode;
	struct aetherfs_inode_info *ei;
	struct aetherfs_inode *raw_inode;
	struct buffer_head *bh;
	int err, i;
	unsigned long ino;

	if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode) && !S_ISCHR(mode) && !S_ISBLK(mode))
		return ERR_PTR(-EINVAL);

	ino = info->s_next_ino++;
	if (ino < AETHERFS_ROOT_INO + 1)
		ino = AETHERFS_ROOT_INO + 1;

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_mode = mode;
	inode->i_ino = ino;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	set_nlink(inode, 1);
	i_size_write(inode, 0);
	inode->i_blocks = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);

	ei = AETHERFS_INODE(inode);
	ei->i_flags = 0;
	ei->i_extent_root = 0;

	inode->i_op = NULL;
	inode->i_fop = NULL;
	inode->i_data.a_ops = NULL;

	if (S_ISREG(mode)) {
		inode->i_op = &aetherfs_file_inops;
		inode->i_fop = &aetherfs_file_operations;
		inode->i_data.a_ops = &aetherfs_aops;
	} else if (S_ISDIR(mode)) {
		inode->i_op = &aetherfs_dir_inops;
		inode->i_fop = &aetherfs_dir_operations;
		inc_nlink(dir);
	} else if (S_ISLNK(mode)) {
		inode->i_op = &aetherfs_symlink_inops;
	} else {
		inode->i_op = &aetherfs_file_inops;
		inode->i_fop = &aetherfs_file_operations;
		init_special_inode(inode, mode, rdev);
	}

	insert_inode_hash(inode);
	mark_inode_dirty(inode);

	return inode;
}

void aetherfs_free_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);

	if (!sb || !info)
		return;

	if (atomic_read(&inode->i_count) > 1)
		return;

	info->s_free_inodes++;
	mark_inode_dirty(inode);
}

int aetherfs_init_inode_cache(void)
{
	aetherfs_inode_cache = kmem_cache_create("aetherfs_inode_cache",
		sizeof(struct aetherfs_inode_info), 0,
		SLAB_RECLAIM_ACCOUNT, NULL);

	if (!aetherfs_inode_cache)
		return -ENOMEM;

	return 0;
}

void aetherfs_destroy_inode_cache(void)
{
	if (aetherfs_inode_cache)
		kmem_cache_destroy(aetherfs_inode_cache);
}