#ifndef AETHERFS_FORMAT_H
#define AETHERFS_FORMAT_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#endif

#if defined(__GNUC__)
#define AETHERFS_PACKED __attribute__((packed))
#else
#define AETHERFS_PACKED
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define aetherfs_cpu_to_le16(v) ((uint16_t)(v))
#define aetherfs_cpu_to_le32(v) ((uint32_t)(v))
#define aetherfs_cpu_to_le64(v) ((uint64_t)(v))
#define aetherfs_le16_to_cpu(v) ((uint16_t)(v))
#define aetherfs_le32_to_cpu(v) ((uint32_t)(v))
#define aetherfs_le64_to_cpu(v) ((uint64_t)(v))
#else
#define aetherfs_cpu_to_le16(v) __builtin_bswap16((uint16_t)(v))
#define aetherfs_cpu_to_le32(v) __builtin_bswap32((uint32_t)(v))
#define aetherfs_cpu_to_le64(v) __builtin_bswap64((uint64_t)(v))
#define aetherfs_le16_to_cpu(v) __builtin_bswap16((uint16_t)(v))
#define aetherfs_le32_to_cpu(v) __builtin_bswap32((uint32_t)(v))
#define aetherfs_le64_to_cpu(v) __builtin_bswap64((uint64_t)(v))
#endif

#define AETHERFS_MAGIC              0x41455448U
#define AETHERFS_VERSION            5U
#define AETHERFS_MIN_BLOCK_SIZE     4096U
#define AETHERFS_DEF_BLOCK_SIZE     4096U
#define AETHERFS_DEF_BLOCK_SIZE_SSD 16384U
#define AETHERFS_MAX_BLOCK_SIZE     65536U
#define AETHERFS_INODE_SIZE         512U
#define AETHERFS_INODE_INLINE_SIZE  372U
#define AETHERFS_MAX_NAMELEN        255U
#define AETHERFS_MAX_SYMLINK        256U
#define AETHERFS_EXTENT_LOG_MAX     102U
#define AETHERFS_BTREE_MAX          128U
#define AETHERFS_BITMAP_HEADER_SIZE 16U
#define AETHERFS_BITMAP_BYTES       (AETHERFS_DEF_BLOCK_SIZE - AETHERFS_BITMAP_HEADER_SIZE)
#define AETHERFS_BITMAP_BITS        (AETHERFS_BITMAP_BYTES * 8U)
#define AETHERFS_BLOCKS_PER_AG      2097152U
#define AETHERFS_BLOCKS_PER_GROUP   AETHERFS_BLOCKS_PER_AG
#define AETHERFS_INODES_PER_GROUP   256U
#define AETHERFS_MAX_INODES         AETHERFS_BITMAP_BITS
#define AETHERFS_CRC_SEED           0xFFFFFFFFU
#define AETHERFS_MAX_DEVICES        8U
#define AETHERFS_MAX_SNAPSHOTS      256U

#define AETHERFS_SSD_MAX_HOLE_PUNCH_LEN    65536U
#define AETHERFS_SSD_DISCARD_GRANULARITY   4096U
#define AETHERFS_SSD_MAX_DISCARD_CHUNK    134217728U

#define AETHERFS_COW_MODE           0U
#define AETHERFS_OVERWRITE_MODE     1U
#define AETHERFS_APPEND_MODE        2U

#define AETHERFS_F_COW              0x0001U
#define AETHERFS_F_COMPRESSED       0x0002U
#define AETHERFS_F_ENCRYPTED        0x0004U
#define AETHERFS_F_REFLINKED        0x0008U
#define AETHERFS_F_VERITY           0x0010U
#define AETHERFS_F_APPEND           0x0020U
#define AETHERFS_F_IMMUTABLE        0x0040U
#define AETHERFS_F_INLINE           0x0080U
#define AETHERFS_F_SMALL            0x0100U

#define AETHERFS_EXT_COW            0x0001U
#define AETHERFS_EXT_SHARED         0x0002U
#define AETHERFS_EXT_UNWRITTEN      0x0004U

#define AETHERFS_SNAP_READONLY      0x0001U
#define AETHERFS_SNAP_MERGING       0x0002U

#define AETHERFS_DEV_ONLINE         0x0001U
#define AETHERFS_DEV_FAILED         0x0002U
#define AETHERFS_DEV_READONLY       0x0004U

#define AETHERFS_DEV_CLASS_NONE        0x00000000U
#define AETHERFS_DEV_CLASS_FAST        0x00000001U
#define AETHERFS_DEV_CLASS_CAPACITY    0x00000002U
#define AETHERFS_DEV_CLASS_ARCHIVAL    0x00000004U
#define AETHERFS_DEV_CLASS_METADATA    0x00000008U
#define AETHERFS_DEV_CLASS_LOG         0x00000010U
#define AETHERFS_DEV_CLASS_CACHE       0x00000020U

#define AETHERFS_REGION_META        1U
#define AETHERFS_REGION_DATA        2U
#define AETHERFS_REGION_LOG         3U
#define AETHERFS_REGION_SCRUB       4U

#define AETHERFS_DG_LINEAR          0U
#define AETHERFS_DG_RAID0           1U
#define AETHERFS_DG_RAID5           2U

#define AETHERFS_POOL_MAGIC         0x504F4F4CU
#define AETHERFS_POOL_VERSION       1U
#define AETHERFS_CHECKPOINT_ROOT_MAGIC 0x43505254U
#define AETHERFS_CHECKPOINT_ROOT_VERSION 1U
#define AETHERFS_POOL_PROFILE_SINGLE 0U
#define AETHERFS_POOL_PROFILE_MIRROR 1U
#define AETHERFS_POOL_PROFILE_STRIPE 2U
#define AETHERFS_POOL_PROFILE_PARITY 3U
#define AETHERFS_POOL_PROFILE_TIERED 4U

#define AETHERFS_POOL_MEMBER_DATA      0x00000001U
#define AETHERFS_POOL_MEMBER_METADATA  0x00000002U
#define AETHERFS_POOL_MEMBER_LOG       0x00000004U
#define AETHERFS_POOL_MEMBER_CACHE     0x00000008U

#define AETHERFS_JOURNAL_RECOVERING 0x0001U
#define AETHERFS_JOURNAL_FAST_COMMIT 0x0002U

#define AETHERFS_JOURNAL_META       1U
#define AETHERFS_JOURNAL_DATA       2U
#define AETHERFS_JOURNAL_ALLOC      3U
#define AETHERFS_JOURNAL_FREE       4U

#define AETHERFS_POOL_LABEL_BLOCK   0U
#define AETHERFS_SUPERBLOCK_BLOCK   1U
#define AETHERFS_ROOT_INODE_BLOCK   2U
#define AETHERFS_ROOT_DIR_BLOCK     3U
#define AETHERFS_JOURNAL_BLOCK      4U
#define AETHERFS_INODE_BITMAP_BLOCK 5U
#define AETHERFS_FREE_EXTENT_BLOCK  6U
#define AETHERFS_CHECKPOINT_ROOT_BLOCK 7U
#define AETHERFS_DATA_START_BLOCK   8U
#define AETHERFS_BOOTSTRAP_BLOCKS   AETHERFS_DATA_START_BLOCK
#define AETHERFS_ROOT_INO           16U
#define AETHERFS_DIR_ENTRY_BASE_SIZE 14U
#define AETHERFS_FT_UNKNOWN         0U
#define AETHERFS_FT_REG_FILE        1U
#define AETHERFS_FT_DIR             2U
#define AETHERFS_JOURNAL_DIRTY      0x00000001U

#define AETHERFS_INODES_PER_BLOCK(block_size) ((uint32_t)((block_size) / AETHERFS_INODE_SIZE))
#define AETHERFS_DIR_REC_LEN(name_len) ((uint16_t)((AETHERFS_DIR_ENTRY_BASE_SIZE + (name_len) + 3U) & ~3U))
#define AETHERFS_CHECKSUM_BLOCK_HEADER_SIZE 16U
#define AETHERFS_CHECKSUMS_PER_BLOCK ((AETHERFS_DEF_BLOCK_SIZE - \
    AETHERFS_CHECKSUM_BLOCK_HEADER_SIZE) / sizeof(uint32_t))

struct aetherfs_super {
    uint32_t s_magic;
    uint32_t s_version;
    uint32_t s_blocksize;
    uint64_t s_blocks_count;
    uint64_t s_free_blocks;
    uint32_t s_blocks_per_ag;
    uint32_t s_ag_count;
    uint32_t s_device_count;
    uint16_t s_mode;
    uint16_t s_features;
    uint32_t s_checksum;
    uint8_t  s_uuid[16];
    uint32_t s_checkpoint_num;
    uint32_t s_checkpoint_mask;
    uint64_t s_checkpoint[8];
    uint64_t s_journal_addr;
    uint64_t s_root_inode;
    uint32_t s_generation;
    uint32_t s_inodes_count;
    uint64_t s_meta_tree;
    uint64_t s_extent_tree;
    uint64_t s_free_tree;
    uint64_t s_snap_tree;
    uint64_t s_scrub_journal;
    uint64_t s_health_journal;
    uint64_t s_reserved2[6];
} AETHERFS_PACKED;

struct aetherfs_inode {
    uint16_t i_mode;
    uint16_t i_links_count;
    uint32_t i_uid;
    uint32_t i_gid;
    uint64_t i_size;
    uint64_t i_blocks;
    uint64_t i_atime;
    uint64_t i_ctime;
    uint64_t i_mtime;
    uint64_t i_extent_root;
    uint32_t i_nlink;
    uint32_t i_flags;
    uint64_t i_xattr;
    uint64_t i_reflink;
    uint64_t i_parent;
    uint64_t i_snapshot_id;
    uint32_t i_checksum;
    uint32_t i_inline_len;
    uint64_t i_data_gen;
    uint8_t  i_data[24];
    uint8_t  i_inline[AETHERFS_INODE_INLINE_SIZE];
} AETHERFS_PACKED;

struct aetherfs_extent {
    uint64_t e_lstart;
    uint64_t e_pstart;
    uint64_t e_csum_start;
    uint64_t e_len;
    uint32_t e_flags;
    uint32_t e_reserved;
} AETHERFS_PACKED;

struct aetherfs_extent_node {
    uint64_t en_next;
    uint32_t en_count;
    uint32_t en_checksum;
    struct aetherfs_extent en_extents[AETHERFS_EXTENT_LOG_MAX];
} AETHERFS_PACKED;

struct aetherfs_checksum_block {
    uint64_t cb_next;
    uint32_t cb_entries;
    uint32_t cb_checksum;
    uint32_t cb_sums[AETHERFS_CHECKSUMS_PER_BLOCK];
} AETHERFS_PACKED;

struct aetherfs_dir_entry {
    uint64_t ino;
    uint16_t rec_len;
    uint16_t name_len;
    uint8_t  file_type;
    uint8_t  checksum;
    char     name[];
} AETHERFS_PACKED;

struct aetherfs_cow_header {
    uint64_t ch_root;
    uint64_t ch_old_root;
    uint32_t ch_generation;
    uint32_t ch_checksum;
    uint64_t ch_snapshot_id;
} AETHERFS_PACKED;

struct aetherfs_snapshot_ref {
    uint64_t sr_id;
    uint64_t sr_gen_time;
    uint64_t sr_root_inode;
    uint64_t sr_extent_tree;
    uint64_t sr_meta_tree;
    uint32_t sr_refcount;
    uint32_t sr_flags;
    uint32_t sr_reserved[4];
} AETHERFS_PACKED;

struct aetherfs_scrub_record {
    uint64_t sc_lblk;
    uint64_t sc_pblk;
    uint32_t sc_length;
    uint32_t sc_gen;
    uint64_t sc_next;
} AETHERFS_PACKED;

struct aetherfs_health_entry {
    uint64_t he_blocknr;
    uint32_t he_gen;
    uint32_t he_error;
    uint64_t he_timestamp;
    uint32_t he_reserved[4];
} AETHERFS_PACKED;

struct aetherfs_intent_log {
    uint64_t il_start;
    uint64_t il_end;
    uint64_t il_head;
    uint64_t il_commit;
    uint32_t il_total;
    uint32_t il_outstanding;
    uint32_t il_checksum;
    uint32_t il_reserved;
} AETHERFS_PACKED;

struct aetherfs_bitmap_block {
    uint32_t bb_checksum;
    uint32_t bb_total_bits;
    uint32_t bb_set_bits;
    uint32_t bb_reserved;
    uint8_t  bb_bits[AETHERFS_BITMAP_BYTES];
} AETHERFS_PACKED;

struct aetherfs_pool_member {
    uint8_t  pm_uuid[16];
    uint64_t pm_total_blocks;
    uint64_t pm_data_blocks;
    uint32_t pm_class_mask;
    uint32_t pm_role_mask;
    uint32_t pm_index;
    uint32_t pm_reserved;
} AETHERFS_PACKED;

struct aetherfs_pool_label {
    uint32_t pl_magic;
    uint32_t pl_version;
    uint32_t pl_checksum;
    uint32_t pl_profile;
    uint32_t pl_member_count;
    uint32_t pl_member_index;
    uint32_t pl_stripe_blocks;
    uint32_t pl_flags;
    uint32_t pl_metadata_mask;
    uint32_t pl_data_mask;
    uint32_t pl_log_mask;
    uint32_t pl_cache_mask;
    uint64_t pl_logical_blocks;
    uint64_t pl_logical_data_blocks;
    uint8_t  pl_pool_uuid[16];
    struct aetherfs_pool_member pl_members[AETHERFS_MAX_DEVICES];
    uint64_t pl_reserved[45];
} AETHERFS_PACKED;

struct aetherfs_checkpoint_root {
    uint32_t cr_magic;
    uint32_t cr_version;
    uint32_t cr_checksum;
    uint32_t cr_generation;
    uint32_t cr_ag_count;
    uint32_t cr_region_count;
    uint64_t cr_super_block;
    uint64_t cr_root_inode_block;
    uint64_t cr_root_dir_block;
    uint64_t cr_journal_block;
    uint64_t cr_inode_bitmap_block;
    uint64_t cr_free_space_root;
    uint64_t cr_metadata_tree_root;
    uint64_t cr_extent_tree_root;
    uint64_t cr_snapshot_root;
    uint64_t cr_scrub_journal;
    uint64_t cr_health_journal;
    uint64_t cr_metadata_region_start;
    uint64_t cr_metadata_region_end;
    uint64_t cr_data_region_start;
    uint64_t cr_data_region_end;
    uint64_t cr_special_metadata_root;
    uint64_t cr_cache_root;
    uint64_t cr_parity_root;
    uint64_t cr_reserved[491];
} AETHERFS_PACKED;

struct aetherfs_btree_key {
    uint64_t k_ino;
    uint64_t k_offset;
    uint64_t k_value;
    uint32_t k_len;
    uint32_t k_flags;
    uint32_t k_checksum;
} AETHERFS_PACKED;

struct aetherfs_btree_node {
    uint64_t bn_next;
    uint32_t bn_level;
    uint32_t bn_count;
    uint32_t bn_checksum;
    uint32_t bn_reserved;
    uint8_t  bn_data[];
} AETHERFS_PACKED;

#ifndef __KERNEL__
_Static_assert(sizeof(struct aetherfs_inode) == AETHERFS_INODE_SIZE,
    "aetherfs inode must remain 512 bytes");
_Static_assert(sizeof(struct aetherfs_super) < AETHERFS_DEF_BLOCK_SIZE,
    "aetherfs superblock must fit in one default block");
_Static_assert(sizeof(struct aetherfs_bitmap_block) == AETHERFS_DEF_BLOCK_SIZE,
    "aetherfs bitmap block must fit one block");
_Static_assert(sizeof(struct aetherfs_extent_node) == AETHERFS_DEF_BLOCK_SIZE,
    "aetherfs extent node must fit one block");
_Static_assert(sizeof(struct aetherfs_checksum_block) == AETHERFS_DEF_BLOCK_SIZE,
    "aetherfs checksum block must fit one block");
_Static_assert(sizeof(struct aetherfs_pool_label) <= AETHERFS_DEF_BLOCK_SIZE,
    "aetherfs pool label must fit one block");
_Static_assert(sizeof(struct aetherfs_checkpoint_root) == AETHERFS_DEF_BLOCK_SIZE,
    "aetherfs checkpoint root must fit one block");
#endif

static inline uint32_t aetherfs_crc32c(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = crc ^ 0xFFFFFFFFU;

    while (len--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i)
            c = (c >> 1) ^ ((c & 1U) ? 0xEDB88320U : 0U);
    }

    return c ^ 0xFFFFFFFFU;
}

static inline uint32_t aetherfs_crc32c_data(const void *data, size_t len)
{
    return aetherfs_crc32c(AETHERFS_CRC_SEED, data, len);
}

static inline uint32_t aetherfs_super_checksum(const struct aetherfs_super *super)
{
    struct aetherfs_super tmp;

    memcpy(&tmp, super, sizeof(tmp));
    tmp.s_checksum = 0;
    return aetherfs_crc32c_data(&tmp, sizeof(tmp));
}

static inline uint32_t aetherfs_inode_checksum(const struct aetherfs_inode *inode)
{
    struct aetherfs_inode tmp;

    memcpy(&tmp, inode, sizeof(tmp));
    tmp.i_checksum = 0;
    return aetherfs_crc32c_data(&tmp, sizeof(tmp));
}

static inline uint32_t aetherfs_bitmap_checksum(const struct aetherfs_bitmap_block *bitmap)
{
    struct aetherfs_bitmap_block tmp;

    memcpy(&tmp, bitmap, sizeof(tmp));
    tmp.bb_checksum = 0;
    return aetherfs_crc32c_data(&tmp, sizeof(tmp));
}

static inline uint32_t aetherfs_journal_checksum(const struct aetherfs_intent_log *journal)
{
    return aetherfs_crc32c_data(journal, offsetof(struct aetherfs_intent_log, il_checksum));
}

static inline uint32_t aetherfs_extent_node_checksum(const struct aetherfs_extent_node *node)
{
    struct aetherfs_extent_node tmp;

    memcpy(&tmp, node, sizeof(tmp));
    tmp.en_checksum = 0;
    return aetherfs_crc32c_data(&tmp, sizeof(tmp));
}

static inline uint32_t aetherfs_checksum_block_checksum(
    const struct aetherfs_checksum_block *block)
{
    struct aetherfs_checksum_block tmp;

    memcpy(&tmp, block, sizeof(tmp));
    tmp.cb_checksum = 0;
    return aetherfs_crc32c_data(&tmp, sizeof(tmp));
}

static inline uint32_t aetherfs_pool_label_checksum(const struct aetherfs_pool_label *label)
{
    struct aetherfs_pool_label tmp;

    memcpy(&tmp, label, sizeof(tmp));
    tmp.pl_checksum = 0;
    return aetherfs_crc32c_data(&tmp, sizeof(tmp));
}

static inline uint32_t aetherfs_checkpoint_root_checksum(
    const struct aetherfs_checkpoint_root *root)
{
    struct aetherfs_checkpoint_root tmp;

    memcpy(&tmp, root, sizeof(tmp));
    tmp.cr_checksum = 0;
    return aetherfs_crc32c_data(&tmp, sizeof(tmp));
}

#endif
