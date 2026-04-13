#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <linux/mpage.h>
#include <linux/namei.h>
#include <linux/seq_file.h>
#include <linux/exportfs.h>
#include <linux/crc32c.h>
#include <linux/rwsem.h>
#include <linux/delay.h>

#include "aetherfs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AetherFS Team");
MODULE_DESCRIPTION("AetherFS - Next-generation filesystem");
MODULE_VERSION("1.0.0");

static struct kmem_cache *aetherfs_inode_cachep;
static struct kmem_cache *aetherfs_extent_cachep;
static struct kmem_cache *aetherfs_xattr_cachep;

static const struct super_operations aetherfs_sops;
static const struct inode_operations aetherfs_dir_inops;
static const struct inode_operations aetherfs_file_inops;
static const struct inode_operations aetherfs_symlink_inops;
static const struct file_operations aetherfs_dir_fops;
static const struct file_operations aetherfs_file_fops;
static const struct address_space_operations aetherfs_aops;
static const struct export_operations aetherfs_export_ops;

static int aetherfs_mount_count = 0;
static int aetherfs_mount_options = 0;

#define AETHERFS_MOUNT_NOBARRIER   1
#define AETHERFS_MOUNT_NOCROSSMNT  2
#define AETHERFS_MOUNT_ERRORS_CONT 4
#define AETHERFS_MOUNT_ERRORS_RO   8
#define AETHERFS_MOUNT_ERRORS_PANIC 16
#define AETHERFS_MOUNT_GRPID        32
#define AETHERFS_MOUNT_DEBUG       64

static int aetherfs_sb_write_super(struct super_block *sb)
{
	struct buffer_head *bh;
	struct aetherfs_super *es;
	int rc = 0;

	if (!sb)
		return -EINVAL;

	es = AETH_SB(sb)->s_es;
	if (!es)
		return -EINVAL;

	es->s_free_blocks = cpu_to_le64(AETH_SB(sb)->s_free_blocks);
	es->s_checksum = cpu_to_le32(aetherfs_crc32(es, sizeof(*es) - 4));

	bh = AETH_SB(sb)->s_sbh;
	if (!bh)
		return -EINVAL;

	lock_buffer(bh);
	mark_buffer_dirty(bh);
	if (aetherfs_mount_options & AETHERFS_MOUNT_NOBARRIER)
		unlock_buffer(bh);
	else {
		sync_dirty_buffer(bh);
		unlock_buffer(bh);
	}

	return rc;
}

static int aetherfs_sync_fs(struct super_block *sb, int wait)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);

	if (!info)
		return 0;

	write_lock(&info->s_lock);
	write_unlock(&info->s_lock);

	if (wait)
		aetherfs_sb_write_super(sb);

	return 0;
}

static int aetherfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct buffer_head *bh = NULL;
	struct aetherfs_inode *raw_inode = NULL;
	struct aetherfs_sb_info *info = AETH_SB(inode->i_sb);
	int err = 0;

	if (!inode || !info)
		return -EINVAL;

	if (inode->i_ino < AETHERFS_ROOT_INO)
		return -EINVAL;

	bh = sb_bread(inode->i_sb, info->s_es->s_root_inode);
	if (!bh)
		return -EIO;

	raw_inode = (struct aetherfs_inode *)bh->b_data;
	if (!raw_inode) {
		err = -EIO;
		goto out;
	}

	raw_inode->i_size = cpu_to_le64(i_size_read(inode));
	raw_inode->i_atime = cpu_to_le64(inode->i_atime.tv_sec);
	raw_inode->i_ctime = cpu_to_le64(inode->i_ctime.tv_sec);
	raw_inode->i_mtime = cpu_to_le64(inode->i_mtime.tv_sec);

	set_buffer_dirty(bh);

out:
	brelse(bh);
	return err;
}

static void aetherfs_put_super(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);

	if (!sb || !info)
		return;

	aetherfs_sb_write_super(sb);

	if (info->s_es)
		kvfree(info->s_es);
	if (info->s_gd)
		kvfree(info->s_gd);
	if (info->s_sbh)
		brelse(info->s_sbh);

	kfree(info);
	sb->s_fs_info = NULL;
}

static int aetherfs_remount(struct super_block *sb, int *flags, char *data)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	int old_options = info->s_mount_opt;
	int rc = 0;

	if (!sb || !info)
		return -EINVAL;

	info->s_mount_opt = aetherfs_mount_options;

	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;

	if (*flags & MS_RDONLY) {
		aetherfs_sync_fs(sb, 1);
	} else {
		info->s_mount_opt |= old_options;
	}

	return rc;
}

static int aetherfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct aetherfs_super *es = AETH_SB(sb)->s_es;
	struct aetherfs_sb_info *info = AETH_SB(sb);

	if (!sb || !es || !buf)
		return -EINVAL;

	buf->f_type = AETHERFS_MAGIC;
	buf->f_bsize = le32_to_cpu(es->s_block_size);
	buf->f_blocks = le64_to_cpu(es->s_blocks_count);
	buf->f_bfree = info->s_free_blocks;
	buf->f_bavail = info->s_free_blocks;
	buf->f_files = le64_to_cpu(es->s_blocks_count) / AETHERFS_INODE_SIZE;
	buf->f_ffree = info->s_free_inodes;
	buf->f_fsid = uuid_to_fsid(es->s_uuid);
	buf->f_namelen = AETHERFS_MAX_NAMELEN;
	buf->f_frsize = le32_to_cpu(es->s_block_size);

	return 0;
}

static int aetherfs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct aetherfs_sb_info *info = AETH_SB(root->d_sb);

	if (!info)
		return 0;

	if (info->s_mount_opt & AETHERFS_MOUNT_NOBARRIER)
		seq_printf(seq, ",nobarrier");
	if (info->s_mount_opt & AETHERFS_MOUNT_DEBUG)
		seq_printf(seq, ",debug");

	return 0;
}

static const struct super_operations aetherfs_sops = {
	.alloc_inode     = aetherfs_alloc_inode,
	.destroy_inode   = aetherfs_destroy_inode,
	.write_inode     = aetherfs_write_inode,
	.put_super       = aetherfs_put_super,
	.sync_fs         = aetherfs_sync_fs,
	.statfs          = aetherfs_statfs,
	.remount         = aetherfs_remount,
	.show_options    = aetherfs_show_options,
	.free_cached_objects = NULL,
};

static int aetherfs_init_inode_cache(void)
{
	aetherfs_inode_cachep = kmem_cache_create("aetherfs_inode_cache",
		sizeof(struct aetherfs_inode_info), 0,
		SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);

	if (!aetherfs_inode_cachep)
		return -ENOMEM;

	aetherfs_extent_cachep = kmem_cache_create("aetherfs_extent_cache",
		sizeof(struct aetherfs_extent_node), 0,
		SLAB_RECLAIM_ACCOUNT, NULL);

	if (!aetherfs_extent_cachep) {
		kmem_cache_destroy(aetherfs_inode_cachep);
		return -ENOMEM;
	}

	return 0;
}

static void aetherfs_destroy_inode_cache(void)
{
	if (aetherfs_inode_cachep)
		kmem_cache_destroy(aetherfs_inode_cachep);
	if (aetherfs_extent_cachep)
		kmem_cache_destroy(aetherfs_extent_cachep);
}

static struct inode *aetherfs_alloc_inode(struct super_block *sb)
{
	struct aetherfs_inode_info *ei;

	ei = kmem_cache_alloc(aetherfs_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;

	inode_init_once(&ei->vfs_inode);
	ei->i_extent = NULL;
	ei->i_flags = 0;
	atomic_set(&ei->i_count, 1);

	return &ei->vfs_inode;
}

static void aetherfs_destroy_inode(struct inode *inode)
{
	struct aetherfs_inode_info *ei = AETHERFS_INODE(inode);

	if (ei->i_extent)
		kmem_cache_free(aetherfs_extent_cachep, ei->i_extent);

	kmem_cache_free(aetherfs_inode_cachep, ei);
}

static int aetherfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *bh = NULL;
	struct aetherfs_super *es = NULL;
	struct aetherfs_sb_info *info = NULL;
	struct inode *root = NULL;
	int s_block_size = AETHERFS_DEF_BLOCK_SIZE;
	int ret = -EINVAL;

	info = kzalloc(sizeof(struct aetherfs_sb_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	sb->s_fs_info = info;
	info->s_sb = sb;

	ret = -EFBIG;
	sb_set_blocksize(sb, s_block_size);

	bh = sb_bread(sb, 1);
	if (!bh) {
		pr_err("AetherFS: unable to read superblock\n");
		goto failed;
	}

	es = (struct aetherfs_super *)bh->b_data;

	if (le32_to_cpu(es->s_magic) != AETHERFS_MAGIC) {
		if (!silent)
			pr_err("AetherFS: magic mismatch\n");
		goto failed;
	}

	info->s_es = es;
	info->s_sbh = bh;
	info->s_blocksize = le32_to_cpu(es->s_block_size);
	info->s_blocksize_bits = ffs(info->s_blocksize) - 1;
	info->s_free_blocks = le64_to_cpu(es->s_free_blocks);
	info->s_groups = le64_to_cpu(es->s_blocks_count) / AETHERFS_BLOCKS_PER_AG;
	info->s_mount_opt = aetherfs_mount_options;

	sb->s_op = &aetherfs_sops;
	sb->s_d_op = &aetherfs_dentry_ops;
	sb->s_export_op = &aetherfs_export_ops;
	sb->s_flags |= MS_NOSEC | MS_NOREMOTELOCK;

	root = iget_locked(sb, AETHERFS_ROOT_INO);
	if (!root) {
		ret = -EACCES;
		goto failed;
	}

	root->i_mode = S_IFDIR | 0755;
	root->i_op = &aetherfs_dir_inops;
	root->i_fop = &aetherfs_dir_fops;
	root->i_atime = root->i_mtime = root->i_ctime = current_time(root);

	aetherfs_inode_info(root)->i_flags = AETHERFS_INODE_DIR;

	unlock_new_inode(root);
	sb->s_root = d_make_root(root);

	if (!sb->s_root) {
		ret = -ENOMEM;
		goto failed;
	}

	aetherfs_mount_count++;
	return 0;

failed:
	brelse(bh);
	if (info)
		kfree(info);
	return ret;
}

static struct dentry *aetherfs_mount(struct file_system_type *fs_type, int flags,
	const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, aetherfs_fill_super);
}

static void aetherfs_kill_sb(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);

	if (!sb)
		return;

	kill_anon_super(sb);
	sb = NULL;

	if (info && info->s_es) {
		memset(info->s_es, 0, sizeof(*info->s_es));
		brelse(info->s_sbh);
	}
}

static struct file_system_type aetherfs_fs_type = {
	.name           = "aetherfs",
	.fs_flags       = FS_REQUIRES_DEV | FS_HAS_SUBMOUNTS,
	.mount          = aetherfs_mount,
	.kill_sb        = aetherfs_kill_sb,
	.fs_parameters  = NULL,
};

static int __init aetherfs_init(void)
{
	int ret;

	pr_info("AetherFS: initializing v1.0.0\n");

	ret = aetherfs_init_inode_cache();
	if (ret) {
		pr_err("AetherFS: failed to initialize inode cache: %d\n", ret);
		return ret;
	}

	ret = register_filesystem(&aetherfs_fs_type);
	if (ret) {
		pr_err("AetherFS: failed to register filesystem: %d\n", ret);
		aetherfs_destroy_inode_cache();
		return ret;
	}

	pr_info("AetherFS: filesystem registered successfully\n");
	return 0;
}

static void __exit aetherfs_exit(void)
{
	unregister_filesystem(&aetherfs_fs_type);
	aetherfs_destroy_inode_cache();
	pr_info("AetherFS: filesystem unregistered\n");
}

module_init(aetherfs_init);
module_exit(aetherfs_exit);