#include "aetherfs.h"
#include <linux/buffer_head.h>

enum aetherfs_checksum_type {
	AETHERFS_CHECKSUM_NONE = 0,
	AETHERFS_CHECKSUM_CRC32C = 1,
	AETHERFS_CHECKSUM_XXH3 = 2,
	AETHERFS_CHECKSUM_BLAKE3 = 3,
};

#define AETHERFS_CHECKSUM_MAGIC 0x434b534d

struct aetherfs_checksum_header {
	__le32 ch_magic;
	__le32 ch_type;
	__le32 ch_checksum;
	__le32 ch_reserved;
};

static enum aetherfs_checksum_type checksum_default = AETHERFS_CHECKSUM_CRC32C;

static inline uint32_t checksum_crc32c_extend(uint32_t crc, const void *data, size_t len)
{
	return aetherfs_crc32c(crc, data, len);
}

int aetherfs_verify_superblock_checksum(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_super *es;
	uint32_t stored, computed;

	if (!info || !info->s_sbh)
		return -EINVAL;

	es = info->s_es;
	stored = le32_to_cpu(es->s_checksum);
	es->s_checksum = 0;
	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, es, 72);
	es->s_checksum = cpu_to_le32(stored);

	return (stored == computed) ? 0 : -EINVAL;
}

int aetherfs_compute_superblock_checksum(struct super_block *sb)
{
	struct aetherfs_sb_info *info = AETH_SB(sb);
	struct aetherfs_super *es;

	if (!info || !info->s_es)
		return -EINVAL;

	es = info->s_es;
	es->s_checksum = 0;
	es->s_checksum = cpu_to_le32(aetherfs_crc32c(AETHERFS_CRC_SEED, es, 72));

	return 0;
}

int aetherfs_verify_inode_checksum(struct inode *inode, struct buffer_head *bh)
{
	struct aetherfs_inode *raw = (struct aetherfs_inode *)bh->b_data;
	uint32_t stored, computed;

	if (!inode || !bh)
		return -EINVAL;

	stored = le32_to_cpu(raw->i_checksum);
	raw->i_checksum = 0;
	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, raw, AETHERFS_INODE_SIZE - 4);
	raw->i_checksum = cpu_to_le32(stored);

	return (stored == computed) ? 0 : -EUCLEAN;
}

int aetherfs_compute_inode_checksum(struct inode *inode, struct buffer_head *bh)
{
	struct aetherfs_inode *raw = (struct aetherfs_inode *)bh->b_data;

	if (!inode || !bh)
		return -EINVAL;

	raw->i_checksum = 0;
	raw->i_checksum = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, raw, AETHERFS_INODE_SIZE - 4));

	return 0;
}

int aetherfs_verify_data_checksum(struct super_block *sb, struct buffer_head *bh,
				   uint64_t blocknr)
{
	struct aetherfs_checksummed_block *cb;
	uint32_t stored, computed;

	if (!sb || !bh)
		return -EINVAL;

	cb = (struct aetherfs_checksummed_block *)bh->b_data;
	if (le32_to_cpu(cb->cb_checksum) == 0)
		return 0;

	stored = le32_to_cpu(cb->cb_checksum);
	cb->cb_checksum = 0;
	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, cb->cb_data,
		sb->s_blocksize - sizeof(struct aetherfs_checksummed_block));
	cb->cb_checksum = cpu_to_le32(stored);

	return (stored == computed) ? 0 : -EUCLEAN;
}

int aetherfs_compute_data_checksum(struct super_block *sb, struct buffer_head *bh,
				    uint64_t blocknr)
{
	struct aetherfs_checksummed_block *cb;

	if (!sb || !bh)
		return -EINVAL;

	cb = (struct aetherfs_checksummed_block *)bh->b_data;
	cb->cb_blocknr = cpu_to_le64(blocknr);
	cb->cb_level = 0;
	cb->cb_checksum = 0;
	cb->cb_checksum = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, cb->cb_data,
			sb->s_blocksize - sizeof(struct aetherfs_checksummed_block)));

	return 0;
}

int aetherfs_verify_directory_entry_checksum(struct aetherfs_dir_entry *de)
{
	uint32_t stored, computed;
	struct aetherfs_dir_entry de_copy;

	if (!de)
		return -EINVAL;

	stored = de->checksum;
	de_copy = *de;
	de_copy.checksum = 0;

	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, &de_copy, 
		sizeof(struct aetherfs_dir_entry) - sizeof(__le32));

	return (stored == computed) ? 0 : -EUCLEAN;
}

int aetherfs_compute_directory_entry_checksum(struct aetherfs_dir_entry *de)
{
	if (!de)
		return -EINVAL;

	de->checksum = 0;
	de->checksum = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, de, 
			sizeof(struct aetherfs_dir_entry) - sizeof(__le32)));

	return 0;
}

int aetherfs_verify_extent_checksum(struct aetherfs_extent_node *node)
{
	uint32_t stored, computed;

	if (!node)
		return -EINVAL;

	stored = le32_to_cpu(node->en_reserved);
	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, node,
		sizeof(struct aetherfs_extent_node) - sizeof(__le32));

	return (stored == computed) ? 0 : -EUCLEAN;
}

int aetherfs_compute_extent_checksum(struct aetherfs_extent_node *node)
{
	if (!node)
		return -EINVAL;

	node->en_reserved = 0;
	node->en_reserved = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, node,
			sizeof(struct aetherfs_extent_node) - sizeof(__le32)));

	return 0;
}

int aetherfs_verify_snapshot_checksum(struct aetherfs_snapshot_ref *snap)
{
	uint32_t stored, computed;

	if (!snap)
		return -EINVAL;

	stored = le32_to_cpu(snap->sr_reserved[3]);
	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, snap,
		sizeof(struct aetherfs_snapshot_ref) - sizeof(__le32));

	return (stored == computed) ? 0 : -EUCLEAN;
}

int aetherfs_compute_snapshot_checksum(struct aetherfs_snapshot_ref *snap)
{
	if (!snap)
		return -EINVAL;

	snap->sr_reserved[3] = 0;
	snap->sr_reserved[3] = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, snap,
			sizeof(struct aetherfs_snapshot_ref) - sizeof(__le32)));

	return 0;
}

int aetherfs_verify_journal_checksum(struct buffer_head *bh)
{
	uint32_t stored, computed;
	struct aetherfs_journal_header *jh;

	if (!bh)
		return -EINVAL;

	jh = (struct aetherfs_journal_header *)bh->b_data;
	stored = le32_to_cpu(jh->j_checksum);
	jh->j_checksum = 0;
	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, jh, 
		sizeof(struct aetherfs_journal_header) - sizeof(__le32));
	jh->j_checksum = cpu_to_le32(stored);

	return (stored == computed) ? 0 : -EUCLEAN;
}

int aetherfs_compute_journal_checksum(struct buffer_head *bh)
{
	struct aetherfs_journal_header *jh;

	if (!bh)
		return -EINVAL;

	jh = (struct aetherfs_journal_header *)bh->b_data;
	jh->j_checksum = 0;
	jh->j_checksum = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, jh, 
			sizeof(struct aetherfs_journal_header) - sizeof(__le32)));

	return 0;
}

int aetherfs_verify_btree_node_checksum(struct aetherfs_btree_node *node,
					 uint32_t size)
{
	uint32_t stored, computed;

	if (!node)
		return -EINVAL;

	stored = le32_to_cpu(node->bn_checksum);
	node->bn_checksum = 0;
	computed = aetherfs_crc32c(AETHERFS_CRC_SEED, node, size - sizeof(__le32));
	node->bn_checksum = cpu_to_le32(stored);

	return (stored == computed) ? 0 : -EUCLEAN;
}

int aetherfs_compute_btree_node_checksum(struct aetherfs_btree_node *node,
					 uint32_t size)
{
	if (!node)
		return -EINVAL;

	node->bn_checksum = 0;
	node->bn_checksum = cpu_to_le32(
		aetherfs_crc32c(AETHERFS_CRC_SEED, node, size - sizeof(__le32)));

	return 0;
}

void aetherfs_set_checksum_type(enum aetherfs_checksum_type type)
{
	checksum_default = type;
}

enum aetherfs_checksum_type aetherfs_get_checksum_type(void)
{
	return checksum_default;
}

int aetherfs_checksum_init(void)
{
	pr_info("AetherFS: checksum module initialized (default: CRC32C)\n");
	return 0;
}

void aetherfs_checksum_exit(void)
{
	pr_info("AetherFS: checksum module exiting\n");
}