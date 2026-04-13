#ifndef _AETHERFS_H
#define _AETHERFS_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <asm/byteorder.h>
#else
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define __le16 uint16_t
#define __le32 uint32_t
#define __le64 uint64_t
#define __u8 uint8_t
#define __u16 uint16_t
#define __u32 uint32_t
#define __u64 uint64_t
#define __u8 uint8_t
#define __le16_to_cpu(x) (x)
#define __le32_to_cpu(x) (x)
#define __le64_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#endif

#define AETHERFS_MAGIC           0x41455448
#define AETHERFS_VERSION       2
#define AETHERFS_MAX_BLOCK_SIZE  65536
#define AETHERFS_MIN_BLOCK_SIZE  4096
#define AETHERFS_DEF_BLOCK_SIZE 4096
#define AETHERFS_DEF_BLOCK_SIZE_SSD 16384
#define AETHERFS_MAX_NAMELEN   255
#define AETHERFS_MAX_SYMLINK    256
#define AETHERFS_INODE_SIZE      512
#define AETHERFS_EXTENT_LOG_MAX 128
#define AETHERFS_BTREE_MAX    128
#define AETHERFS_BLOCKS_PER_AG 2097152
#define AETHERFS_BLOCKS_PER_GROUP AETHERFS_BLOCKS_PER_AG
#define AETHERFS_INODES_PER_GROUP 8192
#define AETHERFS_MAX_INODES       67108864
#define AETHERFS_CRC_SEED      0xFFFFFFFF
#define AETHERFS_COW_MODE      0
#define AETHERFS_OVERWRITE_MODE 1
#define AETHERFS_APPEND_MODE   2

#define AETHERFS_ROOT_INO      16
#define AETHERFS_SUPERBLOCK_BLOCK   1U
#define AETHERFS_ROOT_INODE_BLOCK   2U
#define AETHERFS_ROOT_DIR_BLOCK     3U
#define AETHERFS_JOURNAL_BLOCK      4U
#define AETHERFS_INODE_BITMAP_BLOCK 5U
#define AETHERFS_FREE_EXTENT_BLOCK  6U
#define AETHERFS_DATA_START_BLOCK   7U

#define AETHERFS_SSD_MAX_HOLE_PUNCH_LEN 65536
#define AETHERFS_SSD_DISCARD_GRANULARITY 4096
#define AETHERFS_SSD_MAX_DISCARD_CHUNK   134217728

#define AETHERFS_COW_THRESHOLD 4

enum aetherfs_meta_type {
	AETHERFS_META_INODE = 0,
	AETHERFS_META_EXTENT_TREE,
	AETHERFS_META_DIR_BLOCK,
	AETHERFS_META_ALLOC_MAP,
	AETHERFS_META_SUPERBLOCK,
};

#define AETHERFS_MAX_DEVICES    8
#define AETHERFS_MAX_INLINE  4096

#define AETHERFS_F_COW         0x0001
#define AETHERFS_F_COMPRESSED 0x0002
#define AETHERFS_F_ENCRYPTED  0x0004
#define AETHERFS_F_REFLINKED 0x0008
#define AETHERFS_F_VERITY     0x0010
#define AETHERFS_F_APPEND    0x0020
#define AETHERFS_F_IMMUTABLE 0x0040
#define AETHERFS_F_INLINE   0x0080
#define AETHERFS_F_SMALL   0x0100

#define AETHERFS_INODE_SIZE 512
#define AETHERFS_INODES_PER_BLOCK(block_size) ((uint32_t)((block_size) / AETHERFS_INODE_SIZE))
#define AETHERFS_DIR_REC_LEN(name_len) ((uint16_t)((14 + (name_len) + 3U) & ~3U))

#ifdef __KERNEL__
struct aetherfs_inode_info {
	struct inode vfs_inode;
	u64 i_extent_root;
	u32 i_flags;
	atomic_t i_count;
};
#define AETHERFS_INODE(i) (container_of(i, struct aetherfs_inode_info, vfs_inode))
#else
struct aetherfs_inode_info {
	__le16 i_mode;
	__le16 i_links_count;
	__le32 i_uid;
	__le32 i_gid;
	__le64 i_size;
	__le64 i_blocks;
	__le64 i_atime;
	__le64 i_ctime;
	__le64 i_mtime;
	__le64 i_extent_root;
	__le32 i_nlink;
	__le32 i_flags;
	__le64 i_xattr;
	__le64 i_reflink;
	__le64 i_parent;
	__le64 i_snapshot_id;
	__le32 i_checksum;
	__le32 i_inline_len;
	__le64 i_data_gen;
	__u8 i_data[24];
	__u8 i_inline[372];
};
#define AETHERFS_INODE_SIZE 512
#define AETHERFS_INODES_PER_BLOCK(block_size) ((uint32_t)((block_size) / 512))
#define AETHERFS_DIR_REC_LEN(name_len) ((uint16_t)((14 + (name_len) + 3U) & ~3U))
#endif

struct aetherfs_super {
	__le32 s_magic;
	__le32 s_version;
	__le32 s_blocksize;
	__le64 s_blocks_count;
	__le64 s_free_blocks;
	__le32 s_blocks_per_ag;
	__le32 s_ag_count;
	__le32 s_device_count;
	__le32 s_first_data_block;
	__le16 s_mode;
	__le16 s_features;
	__le32 s_checksum;
	__u8 s_uuid[16];
	__le32 s_checkpoint_num;
	__le32 s_checkpoint_mask;
	__le64 s_checkpoint[8];
	__le64 s_journal_addr;
	__le64 s_root_inode;
	__le32 s_generation;
	__le32 s_inodes_count;
	__le64 s_meta_tree;
	__le64 s_extent_tree;
	__le64 s_free_tree;
	__le64 s_snap_tree;
	__le64 s_scrub_journal;
	__le64 s_health_journal;
	__le64 s_reserved2[6];
};

struct aetherfs_inode {
	__le16 i_mode;
	__le16 i_links_count;
	__le32 i_uid;
	__le32 i_gid;
	__le64 i_size;
	__le64 i_blocks;
	__le64 i_atime;
	__le64 i_ctime;
	__le64 i_mtime;
	__le64 i_extent_root;
	__le32 i_nlink;
	__le32 i_flags;
	__le64 i_xattr;
	__le64 i_reflink;
	__le64 i_parent;
	__le64 i_snapshot_id;
	__le32 i_checksum;
	__le32 i_inline_len;
	__le64 i_data_gen;
	__u8 i_data[24];
	__u8 i_inline[AETHERFS_MAX_INLINE - 100];
};

#define AETHERFS_INODE_EMBEDDED_EXTENTS 4

struct aetherfs_inode_extent_embedded {
	__le64 ee_pstart;
	__le64 ee_lstart;
	__le32 ee_len;
	__le32 ee_flags;
};

struct aetherfs_inode_with_extents {
	struct aetherfs_inode base;
	struct aetherfs_inode_extent_embedded ee_extents[AETHERFS_INODE_EMBEDDED_EXTENTS];
	__le32 ee_count;
	__le32 ee_checksum;
};

struct aetherfs_extent {
	__le64 e_pstart;
	__le64 e_lstart;
	__le32 e_len;
	__le32 e_flags;
#define AETHERFS_EXT_COW      0x0001
#define AETHERFS_EXT_SHARED   0x0002
#define AETHERFS_EXT_UNWRITTEN 0x0004
};

struct aetherfs_extent_node {
	__le64 en_next;
	__le32 en_count;
	__le32 en_reserved;
	struct aetherfs_extent en_extents[AETHERFS_EXTENT_LOG_MAX];
};

struct aetherfs_cow_header {
	__le64 ch_root;
	__le64 ch_old_root;
	__le32 ch_generation;
	__le32 ch_checksum;
	__le64 ch_snapshot_id;
};

struct aetherfs_snapshot_ref {
	__le64 sr_id;
	__le64 sr_gen_time;
	__le64 sr_root_inode;
	__le64 sr_extent_tree;
	__le64 sr_meta_tree;
	__le32 sr_refcount;
	__le32 sr_flags;
#define AETHERFS_SNAP_READONLY 0x0001
#define AETHERFS_SNAP_MERGING  0x0002
	__le32 sr_reserved[4];
};

struct aetherfs_scrub_record {
	__le64 sc_lblk;
	__le64 sc_pblk;
	__le32 sc_length;
	__le32 sc_gen;
	__le64 sc_next;
};

struct aetherfs_health_entry {
	__le64 he_blocknr;
	__le32 he_gen;
	__le32 he_error;
	__le64 he_timestamp;
	__le32 he_reserved[4];
};

struct aetherfs_intent_log {
	__le64 il_start;
	__le64 il_end;
	__le64 il_head;
	__le64 il_commit;
	__le32 il_total;
	__le32 il_outstanding;
	__le32 il_checksum;
	__le32 il_reserved;
};

struct aetherfs_btree_node {
	__le64 bn_next;
	__le32 bn_level;
	__le32 bn_count;
	__le32 bn_checksum;
	__le32 bn_reserved;
	__u8 bn_data[];
};

struct aetherfs_btree_key {
	__le64 k_ino;
	__le64 k_offset;
	__le64 k_value;
	__le32 k_len;
	__le32 k_flags;
	__le32 k_checksum;
};

struct aetherfs_checksummed_block {
	__le32 cb_checksum;
	__le32 cb_blocknr;
	__le16 cb_level;
	__le16 cb_reserved;
	__le64 cb_llog;
	__u8 cb_data[];
};

struct aetherfs_dir_entry {
	__le64 ino;
	__le16 rec_len;
	__le16 name_len;
	__u8 file_type;
	__u8 checksum;
	char name[];
};

struct aetherfs_ag_free {
	__le64 ag_free_addr;
	__le32 ag_free_len;
	__le32 ag_reserved;
	__le64 ag_bitmap_addr;
};

struct aetherfs_device {
	__le32 d_devnr;
	__le32 d_flags;
#define AETHERFS_DEV_ONLINE  0x0001
#define AETHERFS_DEV_FAILED 0x0002
#define AETHERFS_DEV_READONLY 0x0004
	__le64 d_total_blocks;
	__le64 d_free_blocks;
	__le64 d_start_block;
	__le64 d_bad_blocks;
};

struct aetherfs_region {
	__le64 r_start;
	__le64 r_end;
	__le32 r_devnr;
	__le32 r_type;
#define AETHERFS_REGION_META    1
#define AETHERFS_REGION_DATA  2
#define AETHERFS_REGION_LOG   3
#define AETHERFS_REGION_SCRUB 4
	__le32 r_reserved;
	__le64 r_tree_root;
	__le64 r_bitmap;
	__le64 r_free_head;
};

struct aetherfs_alloc_group {
	__le64 ag_start;
	__le64 ag_end;
	__le32 ag_id;
	__le32 ag_type;
	__le64 ag_inode_tree;
	__le64 ag_extent_tree;
	__le64 ag_free_tree;
	__le64 ag_inode_bitmap;
	__le64 ag_data_bitmap;
	__le32 ag_free_inodes;
	__le32 ag_free_blocks;
	__le32 ag_checksum;
	__le32 ag_reserved[3];
};

struct aetherfs_device_group {
	__le32 dg_count;
	__le32 dg_mode;
#define AETHERFS_DG_LINEAR 0
#define AETHERFS_DG_RAID0 1
#define AETHERFS_DG_RAID5 2
	__le32 dg_stripe_size;
	__le64 dg_total_blocks;
	struct aetherfs_device dg_devs[AETHERFS_MAX_DEVICES];
};

struct aetherfs_allocator {
	__le64 a_free_lists;
	__le32 a_preferred_dev;
	__le32 a_max_segments;
	__le32 a_queue_depth;
	__le64 a_async_prealloc;
};

struct aetherfs_journal_log {
	__le64 j_start;
	__le64 j_end;
	__le64 j_head;
	__le64 j_commit;
	__le32 j_checksum;
	__le32 j_txn_max;
	__le32 j_flags;
#define AETHERFS_JOURNAL_RECOVERING 0x0001
#define AETHERFS_JOURNAL_FAST_COMMIT 0x0002
};

struct aetherfs_journal_entry {
	__le64 je_txn_id;
	__le32 je_op;
#define AETHERFS_JOURNAL_META 1
#define AETHERFS_JOURNAL_DATA 2
#define AETHERFS_JOURNAL_ALLOC 3
#define AETHERFS_JOURNAL_FREE 4
	__le32 je_flags;
	__le64 je_blocknr;
	__le32 je_checksum;
	__le32 je_reserved;
	__u8 je_data[];
};

struct aetherfs_journal_header {
	__le64 j_blocknr;
	__le32 j_fs_gen;
	__le32 j_entries;
	__le32 j_checkpoint;
	__le32 j_tail;
	__le32 j_checksum;
};

struct aetherfs_journal_entry_legacy {
	__le64 je_ino;
	__le64 je_block;
	__le32 je_len;
	__le32 je_op;
	__le32 je_checksum;
};

struct aetherfs_group_desc {
	__le32 bg_inode_table;
	__le32 bg_inode_bitmap;
	__le32 bg_data_bitmap;
	__le32 bg_free_inodes;
	__le32 bg_free_blocks;
	__le32 bg_used_dirs;
	__le32 bg_checksum;
	__le64 bg_data_addr;
	__le32 bg_reserved[4];
};

struct aetherfs_sb_info {
	struct super_block  *s_sb;
	struct aetherfs_super *s_es;
	struct buffer_head  *s_sbh;
	struct aetherfs_group_desc *s_gd;
	struct aetherfs_device_group *s_dev_group;
	struct aetherfs_allocator *s_alloc;
	uint32_t            s_blocksize;
	uint32_t            s_blocksize_bits;
	uint32_t            s_mount_opt;
	uint64_t            s_groups;
	uint64_t            s_free_blocks;
	uint64_t            s_free_inodes;
	uint64_t            s_next_ino;
	uint64_t            s_dirty_inodes;
	uint32_t            s_generation;
	uint32_t            s_dev_count;
	uint8_t             s_data_mode;
	uint8_t             s_features;
	int                 s_dirty;
	rwlock_t            s_lock;
	struct mutex        s_mut;
	struct list_head   s_unused_list;
	struct list_head   s_dirty_list;
	atomic_t            s_active_operations;
	void               *s_private;
};

#define AETH_SB(sb) ((struct aetherfs_sb_info *)((sb)->s_fs_info))

static inline uint32_t aetherfs_crc32c(uint32_t crc, const void *buf, size_t len)
{
#ifdef __KERNEL__
	u32 c = crc;
	const u8 *p = buf;
	while (len--) {
		c ^= *p++;
		c = (c >> 1) ^ ((c & 1) ? 0xEDB88320U : 0);
	}
	return ~c;
#else
	uint32_t i, c = crc;
	const uint8_t *b = buf;
	for (i = 0; i < len; i++) {
		c ^= b[i];
		c ^= (c >> 16);
		c ^= (c >> 8);
		c ^= (c >> 4);
		c ^= (c >> 2);
		c ^= (c >> 1);
		c ^= (c & 1) ? 0xEDB88320 : 0;
	}
	return ~c;
#endif
}

static inline uint32_t aetherfs_crc32(const void *buf, size_t len)
{
	return aetherfs_crc32c(0xFFFFFFFF, buf, len);
}

static inline uint32_t aetherfs_checksum_block(const void *block, uint32_t size)
{
	return aetherfs_crc32(block, size);
}

static inline int aetherfs_verify_checksum(const void *block, uint32_t size, uint32_t expected)
{
	return aetherfs_crc32(block, size) == expected;
}

static inline int aetherfs_is_cow_mode(uint32_t flags)
{
	return flags & AETHERFS_F_COW;
}

static inline int aetherfs_is_append_mode(uint32_t flags)
{
	return flags & AETHERFS_F_APPEND;
}

static inline int aetherfs_is_small_file(uint64_t size)
{
	return size <= AETHERFS_MAX_INLINE;
}

static inline uint32_t aetherfs_checksum_metadata(const struct aetherfs_inode *inode)
{
	return aetherfs_crc32(inode, AETHERFS_INODE_SIZE - 4);
}

static inline uint32_t aetherfs_checksum_super(const struct aetherfs_super *super)
{
	return aetherfs_crc32(super, 72);
}

#ifdef __KERNEL__
int aetherfs_init(void);
void aetherfs_exit(void);

int aetherfs_cow_inode(struct inode *inode);
int aetherfs_cow_extent(struct inode *inode, unsigned long lblk, unsigned long len);
int aetherfs_write_with_checksum(struct buffer_head *bh, void *data, size_t len);
int aetherfs_read_verify(struct buffer_head *bh, void *data, size_t len);

int aetherfs_cow_inode_metadata(struct inode *inode);
int aetherfs_cow_extent_tree(struct inode *inode, uint64_t tree_root_old, uint64_t *tree_root_new);
int aetherfs_cow_dir_block(struct inode *dir, uint64_t block_old, uint64_t *block_new);
int aetherfs_cow_alloc_map(struct super_block *sb, uint64_t bitmap_block_old, uint64_t *bitmap_block_new);
int aetherfs_cow_superblock(struct super_block *sb, struct aetherfs_super *old_es, struct aetherfs_super **new_es);
int aetherfs_cow_metadata_update(struct super_block *sb, enum aetherfs_meta_type type, uint64_t old_block, uint64_t *new_block);

int aetherfs_device_init(struct super_block *sb, struct aetherfs_device_group *dg);
int aetherfs_device_failed(struct super_block *sb, int devnr);
int aetherfs_device_online(struct super_block *sb, int devnr);

int aetherfs_btree_insert(struct aetherfs_btree_node **root, struct aetherfs_btree_key *key);
int aetherfs_btree_lookup(struct aetherfs_btree_node *root, uint64_t ino, uint64_t offset, uint64_t *value);
int aetherfs_btree_delete(struct aetherfs_btree_node **root, uint64_t ino, uint64_t offset);
int aetherfs_btree_create(struct aetherfs_btree_node **root, uint32_t node_size);
void aetherfs_btree_destroy(struct aetherfs_btree_node *root);
int aetherfs_btree_insert_extent(struct aetherfs_btree_node **root, uint64_t ino, uint64_t lblock, uint64_t pblock, uint64_t blocks);
int aetherfs_btree_lookup_extent(struct aetherfs_btree_node *root, uint64_t ino, uint64_t lblock, uint64_t *pblock, uint32_t *blocks);
int aetherfs_btree_delete_extent(struct aetherfs_btree_node **root, uint64_t ino, uint64_t lblock);

int aetherfs_journal_start(struct aetherfs_journal_log *log, int nblocks);
int aetherfs_journal_commit(struct aetherfs_journal_log *log, uint64_t txn_id);
int aetherfs_journal_replay(struct aetherfs_journal_log *log);

unsigned long aetherfs_alloc_blocks(struct inode *inode, unsigned long start, unsigned long len);
void aetherfs_free_blocks(struct super_block *sb, unsigned long start, unsigned long len);
uint64_t aetherfs_bmap(struct inode *inode, uint64_t block);

int aetherfs_write_inline(struct inode *inode, const void *data, uint64_t size);
int aetherfs_read_inline(struct inode *inode, void *data, uint64_t size);

int aetherfs_init_folio_ops(void);

#else

int aetherfs_mkfs(char *device, uint64_t blocks, uint32_t dev_count);
int aetherfs_fsck(char *device);

#endif

#define AETHERFS_MAX_EXTENT_TREE_DEPTH 8
#define AETHERFS_EXTENT_NODE_MAX_EXTENTS 64
#define AETHERFS_FREE_EXTENT_TREE_ORDER 32
#define AETHERFS_MAX_FREE_EXTENTS_PER_TREE 4096

struct aetherfs_extent_tree_node {
	__le64 et_pstart;
	__le64 et_lstart;
	__le32 et_len;
	__le32 et_flags;
#define AETHERFS_EXTENT_FLAG_COW      0x0001
#define AETHERFS_EXTENT_FLAG_SHARED  0x0002
#define AETHERFS_EXTENT_FLAG_VERITY  0x0004
#define AETHERFS_EXTENT_FLAG_ENCRYPTED 0x0008
#define AETHERFS_EXTENT_FLAG_COMPRESSED 0x0010
#define AETHERFS_EXTENT_FLAG_SECURE  0x0020
};

struct aetherfs_extent_tree_internal {
	__le64 ei_child[2];
	__le64 ei_lstart;
	__le32 ei_checksum;
	__le32 ei_reserved;
};

struct aetherfs_extent_tree_leaf {
	__le64 el_lstart;
	__le64 el_pstart;
	__le32 el_len;
	__le32 el_flags;
	__le32 el_checksum;
	__le32 el_reserved;
};

struct aetherfs_free_extent {
	__le64 fe_pstart;
	__le64 fe_len;
	__le64 fe_next;
	__le32 fe_checksum;
	__le32 fe_reserved;
};

struct aetherfs_free_extent_tree {
	__le64 ft_root;
	__le32 ft_count;
	__le32 ft_checksum;
	__le64 ft_reserved[4];
};

#define AETHERFS_SNAP_FLAG_AUTHENTICATED 0x0001
#define AETHERFS_SNAP_FLAG_IMMUTABLE     0x0002
#define AETHERFS_SNAP_FLAG_ENCRYPTED     0x0004

struct aetherfs_authenticated_snapshot {
	__le64 as_id;
	__le64 as_timestamp;
	__le64 as_root_inode;
	__le64 as_meta_tree;
	__le64 as_extent_tree;
	__le32 as_flags;
	__le32 as_hmac_algo;
	__u8 as_hmac[64];
	__le64 as_parent_id;
	__le32 as_refcount;
	__le32 as_reserved[3];
	struct list_head as_list;
};

#define AETHERFS_VERITY_MAGIC 0x56455259
#define AETHERFS_VERITY_SHA256 1
#define AETHERFS_VERITY_BLAKE3 2

struct aetherfs_verity_descriptor {
	__le32 vd_magic;
	__le32 vd_version;
	__le32 vd_hash_algo;
	__le32 vd_log_blocksize;
	__le64 vd_data_size;
	__le64 vd_merkle_root;
	__le64 vd_salt_size;
	__u8 vd_salt[];
};

struct aetherfs_verity_metadata {
	__le64 vm_ino;
	__le64 vm_size;
	__le32 vm_algo;
	__le32 vm_block_size;
	__le64 vm_merkle_root;
	__le64 vm_salt_size;
	__le32 vm_signature_size;
	__le32 vm_reserved;
	__u8 vm_data[];
};

#define AETHERFS_XATTR_MAX_SIZE 65536
#define AETHERFS_XATTR_NAME_MAX 255

#define AETHERFS_XATTR_TYPE_USER       1
#define AETHERFS_XATTR_TYPE_TRUSTED    2
#define AETHERFS_XATTR_TYPE_SECURITY   3
#define AETHERFS_XATTR_TYPE_POLICY    4

#define AETHERFS_POLICY_TAG_IMMUTABLE   "security.immutable"
#define AETHERFS_POLICY_TAG_ENCRYPTED   "security.encrypted"
#define AETHERFS_POLICY_TAG_VERIFIED    "security.verified"
#define AETHERFS_POLICY_TAG_AUDIT       "security.audit"
#define AETHERFS_POLICY_TAG_COMPRESSION "policy.compression"
#define AETHERFS_POLICY_TAG_DEDUP       "policy.dedup"
#define AETHERFS_POLICY_TAG_TIER       "policy.tier"

struct aetherfs_xattr_header {
	__le16 xh_name_len;
	__le16 xh_value_len;
	__le16 xh_hash;
	__le16 xh_type;
	__le32 xh_checksum;
	__u8 xh_name[];
};

struct aetherfs_policy_entry {
	__le32 pe_type;
	__le32 pe_flags;
	__le64 pe_max_size;
	__le64 pe_max_extent_size;
	__le32 pe_compression_algo;
	__le32 pe_dedup_mode;
	__le32 pe_tier;
	__le32 pe_reserved[3];
};

struct aetherfs_encryption_key {
	__le64 ek_id;
	__le64 ek_creation_time;
	__le64 ek_expiry_time;
	__le32 ek_algo;
	__le32 ek_keylen;
	__le32 ek_flags;
	__le32 ek_refcount;
	struct list_head ek_list;
	__u8 ek_key[];
};

struct aetherfs_encryption_context {
	__le64 ec_key_id;
	__le64 ec_ino;
	__le32 ec_algo;
	__le32 ec_flags;
	__le64 ec_nonce;
	__le64 ec_reserved;
};

#define AETHERFS_AUDIT_MAX_EVENTS 1024
#define AETHERFS_AUDIT_EVENT_SIZE 256

#define AETHERFS_AUDIT_OP_CREATE     1
#define AETHERFS_AUDIT_OP_DELETE     2
#define AETHERFS_AUDIT_OP_MODIFY     3
#define AETHERFS_AUDIT_OP_SETATTR    4
#define AETHERFS_AUDIT_OP_SETXATTR   5
#define AETHERFS_AUDIT_OP_SNAPSHOT   6
#define AETHERFS_AUDIT_OP_CLONE      7

struct aetherfs_audit_event {
	__le64 ae_timestamp;
	__le64 ae_ino;
	__le64 ae_parent_ino;
	__le32 ae_operation;
	__le32 ae_result;
	__le32 ae_uid;
	__le32 ae_pid;
	__u8 ae_path[256];
	__le32 ae_checksum;
	__le32 ae_reserved[3];
};

struct aetherfs_audit_log {
	__le64 al_start;
	__le64 al_end;
	__le64 al_head;
	__le64 al_tail;
	__le32 al_count;
	__le32 al_checksum;
	__le32 al_flags;
	__le32 al_reserved[3];
};

#define AETHERFS_SECURE_DELETE_CRYPTO  0x0001
#define AETHERFS_SECURE_DELETE_ZEROS   0x0002
#define AETHERFS_SECURE_DELETE_GUTMANN 0x0004

struct aetherfs_secure_delete_hint {
	__le64 sd_ino;
	__le64 sd_size;
	__le32 sd_method;
	__le32 sd_passes;
	__le32 sd_flags;
	__le32 sd_reserved;
};

#define AETHERFS_SUBTREE_FLAG_IMMUTABLE 0x0001
#define AETHERFS_SUBTREE_FLAG_NOEXEC    0x0002
#define AETHERFS_SUBTREE_FLAG_NODUMP    0x0004

struct aetherfs_subtree {
	__le64 st_root_ino;
	__le64 st_parent_ino;
	__le32 st_flags;
	__le32 st_refcount;
	__le64 st_creation_time;
	__le64 st_modification_time;
	__le32 st_reserved[4];
};

#ifdef __KERNEL__

int aetherfs_extent_tree_insert(uint64_t ino, uint64_t lstart, uint64_t pstart, uint32_t len, uint32_t flags);
int aetherfs_extent_tree_lookup(uint64_t ino, uint64_t lstart, uint64_t *pstart, uint32_t *len, uint32_t *flags);
int aetherfs_extent_tree_delete(uint64_t ino, uint64_t lstart, uint32_t len);
int aetherfs_extent_tree_split(uint64_t ino);

int aetherfs_free_extent_tree_insert(uint64_t pstart, uint64_t len);
int aetherfs_free_extent_tree_lookup(uint64_t len, uint64_t *pstart, uint64_t *len_out);
int aetherfs_free_extent_tree_delete(uint64_t pstart, uint64_t len);

int aetherfs_auth_snapshot_create(uint64_t *snapshot_id, const __u8 *hmac_key, size_t key_len);
int aetherfs_auth_snapshot_verify(uint64_t snapshot_id, const __u8 *hmac_key, size_t key_len);

int aetherfs_verity_enable(struct inode *inode);
int aetherfs_verity_disable(struct inode *inode);
int aetherfs_verity_verify(struct inode *inode, loff_t pos, size_t count);

int aetherfs_subtree_set_immutable(uint64_t ino, bool immutable);
int aetherfs_subtree_is_immutable(uint64_t ino);

int aetherfs_encryption_key_add(uint64_t *key_id, const __u8 *key, size_t key_len, uint32_t algo);
int aetherfs_encryption_key_remove(uint64_t key_id);
int aetherfs_encryption_context_create(uint64_t ino, uint64_t key_id);
int aetherfs_encryption_context_destroy(uint64_t ino);

int aetherfs_secure_delete_hint_set(uint64_t ino, uint32_t method, uint32_t flags);

int aetherfs_xattr_set(struct inode *inode, const char *name, const void *value, size_t size, int type);
int aetherfs_xattr_get(struct inode *inode, const char *name, void *buffer, size_t size, int type);
int aetherfs_xattr_list(struct inode *inode, char *buffer, size_t size);

int aetherfs_audit_log_event(uint64_t ino, uint32_t operation, const char *path);

#endif

#endif