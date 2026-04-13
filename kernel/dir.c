#include "aetherfs.h"

static int aetherfs_add_entry(struct dentry *dentry, struct inode *inode)
{
	struct buffer_head *bh = NULL;
	struct aetherfs_dir_entry *de = NULL;
	struct super_block *sb = dentry->d_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	int err = -EINVAL;
	unsigned long block;
	unsigned int b_size = info->s_blocksize;
	unsigned int rec_len;
	int i;

	if (namelen > AETHERFS_MAX_NAMELEN)
		return -ENAMETOOLONG;

	block = info->s_es->s_root_inode;
retry:
	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	de = (struct aetherfs_dir_entry *)bh->b_data;
	for (i = 0; i < b_size; ) {
		if (!de->ino) {
			rec_len = AETHERFS_DIR_REC_LEN(namelen);
			if (i + rec_len <= b_size) {
				de->ino = cpu_to_le64(inode->i_ino);
				de->name_len = namelen;
				de->file_type = DT_UNKNOWN;
				memcpy(de->name, name, namelen);

				mark_buffer_dirty(bh);
				err = 0;
				break;
			}
		}

		i += le16_to_cpu(de->rec_len);
		de = (struct aetherfs_dir_entry *)(bh->b_data + i);
	}

	brelse(bh);

	if (!err)
		info->s_dirty = 1;

	return err;
}

static int aetherfs_delete_entry(struct dentry *dentry)
{
	struct super_block *sb = dentry->d_sb;
	struct aetherfs_dir_entry *de = NULL;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct buffer_head *bh = NULL;
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned int b_size = info->s_blocksize;
	int i, err = -ENOENT;

	bh = sb_bread(sb, info->s_es->s_root_inode);
	if (!bh)
		return -EIO;

	de = (struct aetherfs_dir_entry *)bh->b_data;
	for (i = 0; i < b_size; ) {
		if (!de->ino)
			goto next;

		if (de->name_len == namelen && 
		    !memcmp(de->name, name, namelen)) {
			de->ino = 0;
			mark_buffer_dirty(bh);
			err = 0;
			break;
		}

next:
		i += le16_to_cpu(de->rec_len);
		de = (struct aetherfs_dir_entry *)(bh->b_data + i);
	}

	brelse(bh);
	return err;
}

struct dentry *aetherfs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	struct super_block *sb = dir->i_sb;
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct buffer_head *bh = NULL;
	struct aetherfs_dir_entry *de = NULL;
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned int b_size = info->s_blocksize;
	int i;

	if (namelen > AETHERFS_MAX_NAMELEN)
		return ERR_PTR(-ENAMETOOOONG);

	bh = sb_bread(sb, info->s_es->s_root_inode);
	if (!bh)
		return ERR_PTR(-EIO);

	de = (struct aetherfs_dir_entry *)bh->b_data;
	for (i = 0; i < b_size; ) {
		if (!de->ino)
			goto next;

		if (de->name_len == namelen && 
		    !memcmp(de->name, name, namelen)) {
			inode = aetherfs_iget(sb, le64_to_cpu(de->ino));
			if (IS_ERR(inode))
				goto out;
			break;
		}

next:
		i += le16_to_cpu(de->rec_len);
		de = (struct aetherfs_dir_entry *)(bh->b_data + i);
	}

out:
	brelse(bh);
	d_add(dentry, inode);
	return NULL;
}

static int aetherfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;

	inode = aetherfs_new_inode(dir, S_IFREG | mode, 0);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	aetherfs_add_entry(dentry, inode);
	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	return 0;
}

static int aetherfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int err;

	inode = aetherfs_new_inode(dir, S_IFDIR | mode, 0);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	err = aetherfs_add_entry(dentry, inode);
	if (err) {
		iput(inode);
		return err;
	}

	dir->i_nlink++;
	mark_inode_dirty(dir);
	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	return 0;
}

static int aetherfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	int err;

	err = aetherfs_delete_entry(dentry);
	if (err)
		return err;

	inode->i_nlink--;
	mark_inode_dirty(inode);

	return 0;
}

static int aetherfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	int err;

	err = aetherfs_delete_entry(dentry);
	if (err)
		return err;

	inode->i_nlink--;
	dir->i_nlink--;
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);

	return 0;
}

static int aetherfs_rename(struct inode *old_dir, struct dentry *old_dentry,
	struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = NULL;
	struct dentry *rehash = NULL;
	int err;

	if (flags & RENAME_EXCHANGE) {
		new_inode = new_dentry->d_inode;
		if (new_inode) {
			new_inode->i_nlink--;
			mark_inode_dirty(new_inode);
		}
	}

	err = aetherfs_delete_entry(old_dentry);
	if (err)
		return err;

	if (new_dentry->d_inode) {
		new_inode = new_dentry->d_inode;
		new_inode->i_nlink--;
		mark_inode_dirty(new_inode);
	} else {
		err = aetherfs_add_entry(new_dentry, old_inode);
		if (err)
			goto revert;
	}

	old_inode->i_ctime = current_time(old_inode);
	mark_inode_dirty(old_inode);

	if (old_dir != new_dir) {
		old_dir->i_nlink--;
		mark_inode_dirty(old_dir);

		new_dir->i_nlink++;
		mark_inode_dirty(new_dir);
	}

	return 0;

revert:
	err = aetherfs_add_entry(old_dentry, old_inode);
	if (new_inode)
		new_inode->i_nlink++;
	return err;
}

const struct inode_operations aetherfs_dir_inops = {
	.create         = aetherfs_create,
	.lookup        = aetherfs_lookup,
	.link         = aetherfs_link,
	.unlink        = aetherfs_unlink,
	.mkdir        = aetherfs_mkdir,
	.rmdir        = aetherfs_rmdir,
	.rename       = aetherfs_rename,
	.setattr      = aetherfs_setattr,
	.getattr      = aetherfs_getattr,
	.setxattr     = generic_setxattr,
	.getxattr     = generic_getxattr,
	.listxattr    = generic_listxattr,
	.removexattr  = generic_removexattr,
};

const struct inode_operations aetherfs_file_inops = {
	.setattr      = aetherfs_setattr,
	.getattr      = aetherfs_getattr,
	.setxattr     = generic_setxattr,
	.getxattr     = generic_getxattr,
	.listxattr    = generic_listxattr,
	.removexattr  = generic_removexattr,
};

const struct inode_operations aetherfs_symlink_inops = {
	.readlink     = generic_readlink,
	.follow_link  = page_follow_link,
	.put_link   = page_put_link,
	.setattr      = aetherfs_setattr,
	.getattr      = aetherfs_getattr,
	.setxattr     = generic_setxattr,
	.getxattr     = generic_getxattr,
	.listxattr    = generic_listxattr,
	.removexattr  = generic_removexattr,
};

const struct file_operations aetherfs_dir_operations = {
	.read       = generic_read_dir,
	.iterate   = aetherfs_readdir,
	.open      = aetherfs_dir_open,
	.release   = aetherfs_dir_release,
	.fsync     = aetherfs_sync_file,
};

const struct file_operations aetherfs_file_operations = {
	.read       = new_sync_read,
	.write     = new_sync_write,
	.read_iter  = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
	.mmap       = aetherfs_mmap,
	.open      = aetherfs_file_open,
	.release   = aetherfs_file_release,
	.fsync     = aetherfs_sync_file,
	.splice_read = generic_file_splice_read,
	.splice_write = iter_file_splice_write,
	.unlocked_ioctl = aetherfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = aetherfs_compat_ioctl,
#endif
};

const struct address_space_operations aetherfs_aops = {
	.readpage    = aetherfs_readpage,
	.writepage   = aetherfs_writepage,
	.write_begin = aetherfs_write_begin,
	.write_end  = aetherfs_write_end,
	.bmap       = aetherfs_bmap,
	.direct_IO  = aetherfs_direct_IO,
	.invalidate_folio = aetherfs_invalidate_folio,
	.release_folio = aetherfs_release_folio,
	.migrate_folio = buffer_migrate_folio,
	.is_dirty_writeback = buffer_is_dirty_writeback,
};