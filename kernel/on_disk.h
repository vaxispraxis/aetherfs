#ifndef AETHERFS_ON_DISK_H
#define AETHERFS_ON_DISK_H

#ifdef __GNUC__
#define AETHERFS_PACKED __attribute__((packed))
#else
#define AETHERFS_PACKED
#endif

#ifdef __KERNEL__
#include <linux/types.h>
#else

#ifndef _AETHERFS_TYPES_DEFINED
#define _AETHERFS_TYPES_DEFINED
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

#endif

#define AETHERFS_MAGIC           0x41455448
#define AETHERFS_VERSION         2
#define AETHERFS_MIN_BLOCK_SIZE  4096
#define AETHERFS_DEF_BLOCK_SIZE 4096
#define AETHERFS_DEF_BLOCK_SIZE_SSD 16384
#define AETHERFS_MAX_BLOCK_SIZE 65536
#define AETHERFS_MAX_NAMELEN     255
#define AETHERFS_MAX_SYMLINK     256
#define AETHERFS_MAX_INLINE      4096
#define AETHERFS_EXTENT_LOG_MAX  128
#define AETHERFS_BTREE_MAX       128
#define AETHERFS_BLOCKS_PER_AG   2097152
#define AETHERFS_BLOCKS_PER_GROUP AETHERFS_BLOCKS_PER_AG
#define AETHERFS_INODES_PER_GROUP 8192
#define AETHERFS_MAX_INODES       67108864
#define AETHERFS_CRC_SEED        0xFFFFFFFF
#define AETHERFS_MAX_DEVICES     8
#define AETHERFS_MAX_SNAPSHOTS   256
#define AETHERFS_SSD_MAX_HOLE_PUNCH_LEN 65536
#define AETHERFS_SSD_DISCARD_GRANULARITY 4096
#define AETHERFS_SSD_MAX_DISCARD_CHUNK   134217728

#define AETHERFS_COW_MODE        0
#define AETHERFS_OVERWRITE_MODE  1
#define AETHERFS_APPEND_MODE     2

#define AETHERFS_F_COW         0x0001
#define AETHERFS_F_COMPRESSED 0x0002
#define AETHERFS_F_ENCRYPTED  0x0004
#define AETHERFS_F_REFLINKED  0x0008
#define AETHERFS_F_VERITY     0x0010
#define AETHERFS_F_APPEND    0x0020
#define AETHERFS_F_IMMUTABLE 0x0040
#define AETHERFS_F_INLINE   0x0080
#define AETHERFS_F_SMALL   0x0100

#define AETHERFS_EXT_COW       0x0001
#define AETHERFS_EXT_SHARED    0x0002
#define AETHERFS_EXT_UNWRITTEN 0x0004

#define AETHERFS_SNAP_READONLY 0x0001
#define AETHERFS_SNAP_MERGING  0x0002

#define AETHERFS_DEV_ONLINE     0x0001
#define AETHERFS_DEV_FAILED    0x0002
#define AETHERFS_DEV_READONLY  0x0004

#define AETHERFS_REGION_META   1
#define AETHERFS_REGION_DATA   2
#define AETHERFS_REGION_LOG   3
#define AETHERFS_REGION_SCRUB 4

#define AETHERFS_DG_LINEAR     0
#define AETHERFS_DG_RAID0      1
#define AETHERFS_DG_RAID5      2

#define AETHERFS_JOURNAL_RECOVERING   0x0001
#define AETHERFS_JOURNAL_FAST_COMMIT  0x0002

#define AETHERFS_JOURNAL_META   1
#define AETHERFS_JOURNAL_DATA   2
#define AETHERFS_JOURNAL_ALLOC  3
#define AETHERFS_JOURNAL_FREE   4

#ifndef __KERNEL__
static inline uint32_t aetherfs_crc32c(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = crc ^ 0xFFFFFFFF;
    while (len--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) {
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
        }
    }
    return c ^ 0xFFFFFFFF;
}

static inline uint32_t aetherfs_crc32c_data(const void *data, size_t len)
{
    return aetherfs_crc32c(AETHERFS_CRC_SEED, data, len);
}
#endif

struct aetherfs_super {
    __le32  s_magic;
    __le32  s_version;
    __le32  s_blocksize;
    __le64  s_blocks_count;
    __le64  s_free_blocks;
    __le32  s_blocks_per_ag;
    __le32  s_ag_count;
    __le32  s_device_count;
    __le16  s_mode;
    __le16  s_features;
    __le32  s_checksum;
    __u8    s_uuid[16];
    __le32  s_checkpoint_num;
    __le32  s_checkpoint_mask;
    __le64  s_checkpoint[8];
    __le64  s_journal_addr;
    __le64  s_root_inode;
    __le32  s_generation;
    __le32  s_inodes_count;
    __le64  s_meta_tree;
    __le64  s_extent_tree;
    __le64  s_free_tree;
    __le64  s_snap_tree;
    __le64  s_scrub_journal;
    __le64  s_health_journal;
    __le64  s_reserved2[6];
} AETHERFS_PACKED;

struct aetherfs_inode {
    __le16  i_mode;
    __le16  i_links_count;
    __le32  i_uid;
    __le32  i_gid;
    __le64  i_size;
    __le64  i_blocks;
    __le64  i_atime;
    __le64  i_ctime;
    __le64  i_mtime;
    __le64  i_extent_root;
    __le32  i_nlink;
    __le32  i_flags;
    __le64  i_xattr;
    __le64  i_reflink;
    __le64  i_parent;
    __le64  i_snapshot_id;
    __le32  i_checksum;
    __le32  i_inline_len;
    __le64  i_data_gen;
    __u8    i_data[24];
    __u8    i_inline[AETHERFS_MAX_INLINE - 100];
} AETHERFS_PACKED;

struct aetherfs_extent {
    __le64  e_pstart;
    __le64  e_lstart;
    __le32  e_len;
    __le32  e_flags;
} AETHERFS_PACKED;

struct aetherfs_extent_node {
    __le64  en_next;
    __le32  en_count;
    __le32  en_reserved;
    struct aetherfs_extent en_extents[AETHERFS_EXTENT_LOG_MAX];
} AETHERFS_PACKED;

struct aetherfs_dir_entry {
    __le64  ino;
    __le16  rec_len;
    __le16  name_len;
    __u8    file_type;
    __u8    checksum;
    char    name[];
} AETHERFS_PACKED;

struct aetherfs_cow_header {
    __le64  ch_root;
    __le64  ch_old_root;
    __le32  ch_generation;
    __le32  ch_checksum;
    __le64  ch_snapshot_id;
} AETHERFS_PACKED;

struct aetherfs_snapshot_ref {
    __le64  sr_id;
    __le64  sr_gen_time;
    __le64  sr_root_inode;
    __le64  sr_extent_tree;
    __le64  sr_meta_tree;
    __le32  sr_refcount;
    __le32  sr_flags;
    __le32  sr_reserved[4];
} AETHERFS_PACKED;

struct aetherfs_scrub_record {
    __le64  sc_lblk;
    __le64  sc_pblk;
    __le32  sc_length;
    __le32  sc_gen;
    __le64  sc_next;
} AETHERFS_PACKED;

struct aetherfs_health_entry {
    __le64  he_blocknr;
    __le32  he_gen;
    __le32  he_error;
    __le64  he_timestamp;
    __le32  he_reserved[4];
} AETHERFS_PACKED;

struct aetherfs_intent_log {
    __le64  il_start;
    __le64  il_end;
    __le64  il_head;
    __le64  il_commit;
    __le32  il_total;
    __le32  il_outstanding;
    __le32  il_checksum;
    __le32  il_reserved;
} AETHERFS_PACKED;

struct aetherfs_btree_key {
    __le64  k_ino;
    __le64  k_offset;
    __le64  k_value;
    __le32  k_len;
    __le32  k_flags;
    __le32  k_checksum;
} AETHERFS_PACKED;

struct aetherfs_btree_node {
    __le64  bn_next;
    __le32  bn_level;
    __le32  bn_count;
    __le32  bn_checksum;
    __le32  bn_reserved;
    uint8_t bn_data[1];
} AETHERFS_PACKED;

struct aetherfs_device {
    __le32  d_devnr;
    __le32  d_flags;
    __le64  d_total_blocks;
    __le64  d_free_blocks;
    __le64  d_start_block;
    __le64  d_bad_blocks;
} AETHERFS_PACKED;

struct aetherfs_device_group {
    __le32  dg_count;
    __le32  dg_mode;
    __le32  dg_stripe_size;
    __le64  dg_total_blocks;
    struct aetherfs_device dg_devs[AETHERFS_MAX_DEVICES];
} AETHERFS_PACKED;

struct aetherfs_region {
    __le64  r_start;
    __le64  r_end;
    __le32  r_devnr;
    __le32  r_type;
    __le32  r_reserved;
    __le64  r_tree_root;
    __le64  r_bitmap;
    __le64  r_free_head;
} AETHERFS_PACKED;

struct aetherfs_alloc_group {
    __le64  ag_start;
    __le64  ag_end;
    __le32  ag_id;
    __le32  ag_type;
    __le64  ag_inode_tree;
    __le64  ag_extent_tree;
    __le64  ag_free_tree;
    __le64  ag_inode_bitmap;
    __le64  ag_data_bitmap;
    __le32  ag_free_inodes;
    __le32  ag_free_blocks;
    __le32  ag_checksum;
    __le32  ag_reserved[3];
} AETHERFS_PACKED;

struct aetherfs_journal_header {
    __le64  j_blocknr;
    __le32  j_fs_gen;
    __le32  j_entries;
    __le32  j_checkpoint;
    __le32  j_tail;
    __le32  j_checksum;
} AETHERFS_PACKED;

struct aetherfs_journal_entry {
    __le64  je_txn_id;
    __le32  je_op;
    __le32  je_flags;
    __le64  je_blocknr;
    __le32  je_checksum;
    __le32  je_reserved;
    uint8_t je_data[1];
} AETHERFS_PACKED;

struct aetherfs_group_desc {
    __le32  gd_inode_table;
    __le32  gd_inode_bitmap;
    __le32  gd_data_bitmap;
    __le32  gd_free_inodes;
    __le32  gd_free_blocks;
    __le32  gd_checksum;
    __le64  gd_data_addr;
    __le32  gd_reserved[4];
} AETHERFS_PACKED;

struct aetherfs_fs_identity {
    __le32  magic;
    __le32  version;
    __le32  gen;
    __le64  created;
    __le64  last_mount;
    __le32  mount_count;
    __le32  flags;
    __u8    uuid[16];
    __le32  checksum;
} AETHERFS_PACKED;

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#endif

#ifndef S_IFMT
#define S_IFMT    0xF000
#define S_IFLNK   0xA000
#define S_IFREG   0x8000
#define S_IFDIR   0x4000
#define S_IFCHR   0x2000
#define S_IFBLK   0x6000
#define S_IFIFO   0x1000
#define S_IFSOCK  0xC000
#endif

#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISCHR
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#endif

#ifndef __KERNEL__
static inline uint8_t fs_mode_to_dtype(uint16_t mode)
{
    switch (mode & S_IFMT) {
        case S_IFLNK: return DT_LNK;
        case S_IFREG: return DT_REG;
        case S_IFDIR: return DT_DIR;
        case S_IFCHR: return DT_CHR;
        case S_IFBLK: return DT_BLK;
        case S_IFIFO: return DT_FIFO;
        case S_IFSOCK: return DT_SOCK;
        default: return DT_UNKNOWN;
    }
}
#endif

#endif