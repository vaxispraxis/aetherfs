#include "mkfs.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../kernel/include/uapi/linux/aetherfs_format.h"

struct aetherfs_format_state {
    int fd;
    uint64_t blocks;
    struct stat st;
};

static uint32_t aetherfs_bitmap_count_set(const struct aetherfs_bitmap_block *bitmap)
{
    uint32_t count = 0;

    for (size_t index = 0; index < sizeof(bitmap->bb_bits); ++index)
        count += (uint32_t)__builtin_popcount((unsigned int)bitmap->bb_bits[index]);

    return count;
}

static void aetherfs_bitmap_set(struct aetherfs_bitmap_block *bitmap, uint32_t bit)
{
    bitmap->bb_bits[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
}

static uint32_t aetherfs_min_u32(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

static uint32_t aetherfs_mode_features(uint16_t mode)
{
    switch (mode) {
    case AETHERFS_APPEND_MODE:
        return AETHERFS_F_APPEND;
    case AETHERFS_OVERWRITE_MODE:
        return 0;
    case AETHERFS_COW_MODE:
    default:
        return AETHERFS_F_COW;
    }
}

static int aetherfs_fill_uuid(uint8_t uuid[16])
{
    int fd;
    ssize_t nread;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    nread = read(fd, uuid, 16);
    close(fd);
    if (nread != 16)
        return -EIO;

    uuid[6] = (uint8_t)((uuid[6] & 0x0F) | 0x40);
    uuid[8] = (uint8_t)((uuid[8] & 0x3F) | 0x80);
    return 0;
}

static int aetherfs_write_all(int fd, uint64_t offset, const void *buf, size_t len)
{
    const uint8_t *cursor = buf;
    size_t written = 0;

    while (written < len) {
        ssize_t rc = pwrite(fd, cursor + written, len - written, (off_t)(offset + written));
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (rc == 0)
            return -EIO;
        written += (size_t)rc;
    }

    return 0;
}

static int aetherfs_read_all(int fd, uint64_t offset, void *buf, size_t len)
{
    uint8_t *cursor = buf;
    size_t read_total = 0;

    while (read_total < len) {
        ssize_t rc = pread(fd, cursor + read_total, len - read_total, (off_t)(offset + read_total));
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (rc == 0)
            return -EIO;
        read_total += (size_t)rc;
    }

    return 0;
}

static int aetherfs_zero_block(int fd, uint32_t block)
{
    uint8_t block_data[AETHERFS_DEF_BLOCK_SIZE] = {0};

    return aetherfs_write_all(fd, (uint64_t)block * AETHERFS_DEF_BLOCK_SIZE,
        block_data, sizeof(block_data));
}

static int aetherfs_probe_magic(int fd, bool *matches)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_super *super = (struct aetherfs_super *)block;
    int rc;

    *matches = false;
    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_SUPERBLOCK_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc == -EIO)
        return 0;
    if (rc < 0)
        return rc;

    *matches = aetherfs_le32_to_cpu(super->s_magic) == AETHERFS_MAGIC;
    return 0;
}

static int aetherfs_prepare_target(const struct aetherfs_mkfs_options *options,
    struct aetherfs_format_state *state)
{
    bool exists = access(options->path, F_OK) == 0;
    bool has_magic = false;
    uint64_t bytes = 0;
    int fd;
    int rc;

    fd = open(options->path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -errno;

    if (fstat(fd, &state->st) < 0) {
        rc = -errno;
        close(fd);
        return rc;
    }

    if (!S_ISREG(state->st.st_mode) && !S_ISBLK(state->st.st_mode)) {
        close(fd);
        return -EINVAL;
    }

    if (exists) {
        rc = aetherfs_probe_magic(fd, &has_magic);
        if (rc < 0) {
            close(fd);
            return rc;
        }
        if (has_magic && !options->force) {
            close(fd);
            return -EEXIST;
        }
    }

    if (options->blocks > 0) {
        state->blocks = options->blocks;
        bytes = state->blocks * (uint64_t)options->block_size;

        if (S_ISREG(state->st.st_mode) && ftruncate(fd, (off_t)bytes) < 0) {
            rc = -errno;
            close(fd);
            return rc;
        }
    } else if (S_ISBLK(state->st.st_mode)) {
        uint64_t device_bytes = 0;

        if (ioctl(fd, BLKGETSIZE64, &device_bytes) < 0) {
            rc = -errno;
            close(fd);
            return rc;
        }

        state->blocks = device_bytes / (uint64_t)options->block_size;
    } else if (state->st.st_size > 0) {
        if ((uint64_t)state->st.st_size % options->block_size != 0) {
            close(fd);
            return -EINVAL;
        }
        state->blocks = (uint64_t)state->st.st_size / options->block_size;
    } else {
        close(fd);
        return -EINVAL;
    }

    if (state->blocks < AETHERFS_BOOTSTRAP_BLOCKS) {
        close(fd);
        return -ENOSPC;
    }
    state->fd = fd;
    return 0;
}

static uint8_t aetherfs_dirent_checksum(const struct aetherfs_dir_entry *entry)
{
    uint8_t scratch[AETHERFS_DIR_REC_LEN(AETHERFS_MAX_NAMELEN)] = {0};
    size_t entry_len = AETHERFS_DIR_ENTRY_BASE_SIZE + aetherfs_le16_to_cpu(entry->name_len);
    uint32_t crc;

    memcpy(scratch, entry, entry_len);
    scratch[offsetof(struct aetherfs_dir_entry, checksum)] = 0;
    crc = aetherfs_crc32c_data(scratch, entry_len);
    return (uint8_t)(crc & 0xFFU);
}

static void aetherfs_init_root_dir(uint8_t block[AETHERFS_DEF_BLOCK_SIZE])
{
    struct aetherfs_dir_entry *dot = (struct aetherfs_dir_entry *)block;
    struct aetherfs_dir_entry *dotdot;
    uint16_t dot_len = AETHERFS_DIR_REC_LEN(1);

    memset(block, 0, AETHERFS_DEF_BLOCK_SIZE);

    dot->ino = aetherfs_cpu_to_le64(AETHERFS_ROOT_INO);
    dot->rec_len = aetherfs_cpu_to_le16(dot_len);
    dot->name_len = aetherfs_cpu_to_le16(1);
    dot->file_type = AETHERFS_FT_DIR;
    dot->checksum = 0;
    memcpy(dot->name, ".", 1);
    dot->checksum = aetherfs_dirent_checksum(dot);

    dotdot = (struct aetherfs_dir_entry *)(block + dot_len);
    dotdot->ino = aetherfs_cpu_to_le64(AETHERFS_ROOT_INO);
    dotdot->rec_len = aetherfs_cpu_to_le16((uint16_t)(AETHERFS_DEF_BLOCK_SIZE - dot_len));
    dotdot->name_len = aetherfs_cpu_to_le16(2);
    dotdot->file_type = AETHERFS_FT_DIR;
    dotdot->checksum = 0;
    memcpy(dotdot->name, "..", 2);
    dotdot->checksum = aetherfs_dirent_checksum(dotdot);
}

static void aetherfs_init_super(struct aetherfs_super *super,
    const struct aetherfs_mkfs_options *options, uint64_t blocks)
{
    memset(super, 0, sizeof(*super));

    super->s_magic = aetherfs_cpu_to_le32(AETHERFS_MAGIC);
    super->s_version = aetherfs_cpu_to_le32(AETHERFS_VERSION);
    super->s_blocksize = aetherfs_cpu_to_le32(options->block_size);
    super->s_blocks_count = aetherfs_cpu_to_le64(blocks);
    super->s_free_blocks = aetherfs_cpu_to_le64(blocks - AETHERFS_DATA_START_BLOCK);
    super->s_blocks_per_ag = aetherfs_cpu_to_le32(aetherfs_min_u32((uint32_t)blocks,
        AETHERFS_BLOCKS_PER_AG));
    super->s_ag_count = aetherfs_cpu_to_le32(
        (uint32_t)((blocks + AETHERFS_BLOCKS_PER_AG - 1) / AETHERFS_BLOCKS_PER_AG));
    super->s_device_count = aetherfs_cpu_to_le32(1);
    super->s_mode = aetherfs_cpu_to_le16(options->mode);
    super->s_features = aetherfs_cpu_to_le16((uint16_t)aetherfs_mode_features(options->mode));
    super->s_checkpoint_num = aetherfs_cpu_to_le32(1);
    super->s_checkpoint_mask = aetherfs_cpu_to_le32(1);
    super->s_checkpoint[0] = aetherfs_cpu_to_le64(AETHERFS_CHECKPOINT_ROOT_BLOCK);
    super->s_journal_addr = aetherfs_cpu_to_le64(AETHERFS_JOURNAL_BLOCK);
    super->s_root_inode = aetherfs_cpu_to_le64(AETHERFS_ROOT_INODE_BLOCK);
    super->s_generation = aetherfs_cpu_to_le32(1);
    super->s_inodes_count = aetherfs_cpu_to_le32(AETHERFS_MAX_INODES);
    super->s_meta_tree = aetherfs_cpu_to_le64(AETHERFS_INODE_BITMAP_BLOCK);
    super->s_extent_tree = aetherfs_cpu_to_le64(0);
    super->s_free_tree = aetherfs_cpu_to_le64(AETHERFS_FREE_EXTENT_BLOCK);
    super->s_snap_tree = aetherfs_cpu_to_le64(0);
    super->s_checksum = aetherfs_cpu_to_le32(aetherfs_super_checksum(super));
}

static void aetherfs_init_checkpoint_root(struct aetherfs_checkpoint_root *root,
    const struct aetherfs_super *super, uint64_t blocks)
{
    memset(root, 0, sizeof(*root));
    root->cr_magic = aetherfs_cpu_to_le32(AETHERFS_CHECKPOINT_ROOT_MAGIC);
    root->cr_version = aetherfs_cpu_to_le32(AETHERFS_CHECKPOINT_ROOT_VERSION);
    root->cr_generation = super->s_generation;
    root->cr_ag_count = super->s_ag_count;
    root->cr_region_count = aetherfs_cpu_to_le32(2);
    root->cr_super_block = aetherfs_cpu_to_le64(AETHERFS_SUPERBLOCK_BLOCK);
    root->cr_root_inode_block = super->s_root_inode;
    root->cr_root_dir_block = aetherfs_cpu_to_le64(AETHERFS_ROOT_DIR_BLOCK);
    root->cr_journal_block = super->s_journal_addr;
    root->cr_inode_bitmap_block = super->s_meta_tree;
    root->cr_free_space_root = super->s_free_tree;
    root->cr_metadata_tree_root = super->s_meta_tree;
    root->cr_extent_tree_root = super->s_extent_tree;
    root->cr_snapshot_root = super->s_snap_tree;
    root->cr_scrub_journal = super->s_scrub_journal;
    root->cr_health_journal = super->s_health_journal;
    root->cr_metadata_region_start = aetherfs_cpu_to_le64(AETHERFS_SUPERBLOCK_BLOCK);
    root->cr_metadata_region_end = aetherfs_cpu_to_le64(AETHERFS_CHECKPOINT_ROOT_BLOCK);
    root->cr_data_region_start = aetherfs_cpu_to_le64(AETHERFS_DATA_START_BLOCK);
    root->cr_data_region_end = aetherfs_cpu_to_le64(blocks);
    root->cr_checksum = aetherfs_cpu_to_le32(aetherfs_checkpoint_root_checksum(root));
}

static void aetherfs_init_root_inode(struct aetherfs_inode *inode)
{
    struct timespec now;
    uint64_t ts = 0;

    memset(inode, 0, sizeof(*inode));
    if (clock_gettime(CLOCK_REALTIME, &now) == 0)
        ts = (uint64_t)now.tv_sec;

    inode->i_mode = aetherfs_cpu_to_le16(0040755);
    inode->i_links_count = aetherfs_cpu_to_le16(2);
    inode->i_uid = aetherfs_cpu_to_le32((uint32_t)getuid());
    inode->i_gid = aetherfs_cpu_to_le32((uint32_t)getgid());
    inode->i_size = aetherfs_cpu_to_le64(AETHERFS_DEF_BLOCK_SIZE);
    inode->i_blocks = aetherfs_cpu_to_le64(1);
    inode->i_atime = aetherfs_cpu_to_le64(ts);
    inode->i_ctime = aetherfs_cpu_to_le64(ts);
    inode->i_mtime = aetherfs_cpu_to_le64(ts);
    inode->i_extent_root = aetherfs_cpu_to_le64(AETHERFS_ROOT_DIR_BLOCK);
    inode->i_nlink = aetherfs_cpu_to_le32(2);
    inode->i_flags = aetherfs_cpu_to_le32(AETHERFS_F_COW);
    inode->i_parent = aetherfs_cpu_to_le64(AETHERFS_ROOT_INO);
    inode->i_checksum = aetherfs_cpu_to_le32(aetherfs_inode_checksum(inode));
}

static void aetherfs_init_journal(struct aetherfs_intent_log *journal)
{
    memset(journal, 0, sizeof(*journal));
    journal->il_start = aetherfs_cpu_to_le64(AETHERFS_JOURNAL_BLOCK);
    journal->il_end = aetherfs_cpu_to_le64(AETHERFS_JOURNAL_BLOCK);
    journal->il_head = aetherfs_cpu_to_le64(AETHERFS_JOURNAL_BLOCK);
    journal->il_commit = aetherfs_cpu_to_le64(AETHERFS_JOURNAL_BLOCK);
    journal->il_checksum = aetherfs_cpu_to_le32(aetherfs_journal_checksum(journal));
}

static void aetherfs_init_inode_bitmap(struct aetherfs_bitmap_block *bitmap)
{
    memset(bitmap, 0, sizeof(*bitmap));
    bitmap->bb_total_bits = aetherfs_cpu_to_le32(AETHERFS_MAX_INODES);
    aetherfs_bitmap_set(bitmap, 0);
    bitmap->bb_set_bits = aetherfs_cpu_to_le32(1);
    bitmap->bb_checksum = aetherfs_cpu_to_le32(aetherfs_bitmap_checksum(bitmap));
}

static void aetherfs_init_free_extents(struct aetherfs_extent_node *node, uint64_t blocks)
{
    memset(node, 0, sizeof(*node));
    node->en_count = aetherfs_cpu_to_le32(1);
    node->en_extents[0].e_pstart = aetherfs_cpu_to_le64(AETHERFS_DATA_START_BLOCK);
    node->en_extents[0].e_len = aetherfs_cpu_to_le64(blocks - AETHERFS_DATA_START_BLOCK);
    node->en_checksum = aetherfs_cpu_to_le32(aetherfs_extent_node_checksum(node));
}

static int aetherfs_write_bootstrap(const struct aetherfs_mkfs_options *options,
    const struct aetherfs_format_state *state)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE] = {0};
    struct aetherfs_super bootstrap_super;
    struct aetherfs_super *super = (struct aetherfs_super *)block;
    struct aetherfs_inode *inode = (struct aetherfs_inode *)block;
    struct aetherfs_intent_log *journal = (struct aetherfs_intent_log *)block;
    struct aetherfs_bitmap_block *bitmap = (struct aetherfs_bitmap_block *)block;
    struct aetherfs_extent_node *free_node = (struct aetherfs_extent_node *)block;
    struct aetherfs_checkpoint_root *checkpoint_root =
        (struct aetherfs_checkpoint_root *)block;
    int rc;

    for (uint32_t reserved = 0; reserved < AETHERFS_BOOTSTRAP_BLOCKS; ++reserved) {
        rc = aetherfs_zero_block(state->fd, reserved);
        if (rc < 0)
            return rc;
    }

    memset(block, 0, sizeof(block));
    aetherfs_init_super(super, options, state->blocks);
    rc = aetherfs_fill_uuid(super->s_uuid);
    if (rc < 0)
        return rc;
    super->s_checksum = aetherfs_cpu_to_le32(aetherfs_super_checksum(super));
    memcpy(&bootstrap_super, super, sizeof(bootstrap_super));
    rc = aetherfs_write_all(state->fd,
        (uint64_t)AETHERFS_SUPERBLOCK_BLOCK * AETHERFS_DEF_BLOCK_SIZE, block, sizeof(block));
    if (rc < 0)
        return rc;

    memset(block, 0, sizeof(block));
    aetherfs_init_root_inode(inode);
    rc = aetherfs_write_all(state->fd,
        (uint64_t)AETHERFS_ROOT_INODE_BLOCK * AETHERFS_DEF_BLOCK_SIZE, block, sizeof(block));
    if (rc < 0)
        return rc;

    aetherfs_init_root_dir(block);
    rc = aetherfs_write_all(state->fd,
        (uint64_t)AETHERFS_ROOT_DIR_BLOCK * AETHERFS_DEF_BLOCK_SIZE, block, sizeof(block));
    if (rc < 0)
        return rc;

    memset(block, 0, sizeof(block));
    aetherfs_init_journal(journal);
    rc = aetherfs_write_all(state->fd,
        (uint64_t)AETHERFS_JOURNAL_BLOCK * AETHERFS_DEF_BLOCK_SIZE, block, sizeof(block));
    if (rc < 0)
        return rc;

    memset(block, 0, sizeof(block));
    aetherfs_init_inode_bitmap(bitmap);
    rc = aetherfs_write_all(state->fd,
        (uint64_t)AETHERFS_INODE_BITMAP_BLOCK * AETHERFS_DEF_BLOCK_SIZE, block, sizeof(block));
    if (rc < 0)
        return rc;

    memset(block, 0, sizeof(block));
    aetherfs_init_free_extents(free_node, state->blocks);
    rc = aetherfs_write_all(state->fd,
        (uint64_t)AETHERFS_FREE_EXTENT_BLOCK * AETHERFS_DEF_BLOCK_SIZE, block, sizeof(block));
    if (rc < 0)
        return rc;

    memset(block, 0, sizeof(block));
    aetherfs_init_checkpoint_root(checkpoint_root, &bootstrap_super, state->blocks);
    rc = aetherfs_write_all(state->fd,
        (uint64_t)AETHERFS_CHECKPOINT_ROOT_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0)
        return rc;

    if (fsync(state->fd) < 0)
        return -errno;

    return 0;
}

static int aetherfs_validate_super(int fd, uint64_t blocks)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_super *super = (struct aetherfs_super *)block;
    uint32_t checksum;
    int rc;

    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_SUPERBLOCK_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0)
        return rc;

    if (aetherfs_le32_to_cpu(super->s_magic) != AETHERFS_MAGIC)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_version) != AETHERFS_VERSION)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_blocksize) != AETHERFS_DEF_BLOCK_SIZE)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_blocks_count) != blocks)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_free_blocks) != blocks - AETHERFS_DATA_START_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_checkpoint[0]) != AETHERFS_CHECKPOINT_ROOT_BLOCK)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_inodes_count) != AETHERFS_MAX_INODES)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_meta_tree) != AETHERFS_INODE_BITMAP_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_extent_tree) != 0)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_free_tree) != AETHERFS_FREE_EXTENT_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_snap_tree) != 0)
        return -EINVAL;
    checksum = aetherfs_super_checksum(super);
    if (aetherfs_le32_to_cpu(super->s_checksum) != checksum)
        return -EBADE;

    return 0;
}

static int aetherfs_validate_checkpoint_root(int fd, uint64_t blocks)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_checkpoint_root *root = (struct aetherfs_checkpoint_root *)block;
    uint32_t checksum;
    int rc;

    rc = aetherfs_read_all(fd,
        (uint64_t)AETHERFS_CHECKPOINT_ROOT_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0)
        return rc;

    if (aetherfs_le32_to_cpu(root->cr_magic) != AETHERFS_CHECKPOINT_ROOT_MAGIC)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_version) != AETHERFS_CHECKPOINT_ROOT_VERSION)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_generation) != 1)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_ag_count) !=
        (uint32_t)((blocks + AETHERFS_BLOCKS_PER_AG - 1) / AETHERFS_BLOCKS_PER_AG))
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_region_count) != 2)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_super_block) != AETHERFS_SUPERBLOCK_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_root_inode_block) != AETHERFS_ROOT_INODE_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_root_dir_block) != AETHERFS_ROOT_DIR_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_journal_block) != AETHERFS_JOURNAL_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_inode_bitmap_block) != AETHERFS_INODE_BITMAP_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_free_space_root) != AETHERFS_FREE_EXTENT_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_metadata_tree_root) != AETHERFS_INODE_BITMAP_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_extent_tree_root) != 0)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_snapshot_root) != 0)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_metadata_region_start) != AETHERFS_SUPERBLOCK_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_metadata_region_end) != AETHERFS_CHECKPOINT_ROOT_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_data_region_start) != AETHERFS_DATA_START_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_data_region_end) != blocks)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_special_metadata_root) != 0 ||
        aetherfs_le64_to_cpu(root->cr_cache_root) != 0 ||
        aetherfs_le64_to_cpu(root->cr_parity_root) != 0) {
        return -EINVAL;
    }
    checksum = aetherfs_checkpoint_root_checksum(root);
    if (aetherfs_le32_to_cpu(root->cr_checksum) != checksum)
        return -EBADE;

    return 0;
}

static int aetherfs_validate_root_inode(int fd)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_inode *inode = (struct aetherfs_inode *)block;
    uint32_t checksum;
    int rc;

    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_ROOT_INODE_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0)
        return rc;

    checksum = aetherfs_inode_checksum(inode);
    if (aetherfs_le32_to_cpu(inode->i_checksum) != checksum)
        return -EBADE;
    if (aetherfs_le16_to_cpu(inode->i_mode) != 0040755)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(inode->i_extent_root) != AETHERFS_ROOT_DIR_BLOCK)
        return -EINVAL;

    return 0;
}

static int aetherfs_validate_root_dir(int fd)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_dir_entry *dot = (struct aetherfs_dir_entry *)block;
    struct aetherfs_dir_entry *dotdot;
    int rc;

    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_ROOT_DIR_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0)
        return rc;

    if (aetherfs_le64_to_cpu(dot->ino) != AETHERFS_ROOT_INO)
        return -EINVAL;
    if (aetherfs_le16_to_cpu(dot->name_len) != 1 || memcmp(dot->name, ".", 1) != 0)
        return -EINVAL;
    if (dot->checksum != aetherfs_dirent_checksum(dot))
        return -EBADE;

    dotdot = (struct aetherfs_dir_entry *)(block + aetherfs_le16_to_cpu(dot->rec_len));
    if (aetherfs_le64_to_cpu(dotdot->ino) != AETHERFS_ROOT_INO)
        return -EINVAL;
    if (aetherfs_le16_to_cpu(dotdot->name_len) != 2 || memcmp(dotdot->name, "..", 2) != 0)
        return -EINVAL;
    if (dotdot->checksum != aetherfs_dirent_checksum(dotdot))
        return -EBADE;

    return 0;
}

static int aetherfs_validate_journal(int fd)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_intent_log *journal = (struct aetherfs_intent_log *)block;
    uint32_t checksum;
    int rc;

    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_JOURNAL_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0)
        return rc;

    checksum = aetherfs_journal_checksum(journal);
    if (aetherfs_le32_to_cpu(journal->il_checksum) != checksum)
        return -EBADE;
    if (aetherfs_le32_to_cpu(journal->il_outstanding) != 0)
        return -EUCLEAN;

    return 0;
}

static int aetherfs_validate_bitmap(int fd, uint64_t offset, uint32_t expected_bits,
    uint32_t expected_set_bits)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_bitmap_block *bitmap = (struct aetherfs_bitmap_block *)block;
    uint32_t checksum;
    uint32_t actual_set_bits;
    int rc;

    rc = aetherfs_read_all(fd, offset, block, sizeof(block));
    if (rc < 0)
        return rc;

    if (aetherfs_le32_to_cpu(bitmap->bb_total_bits) != expected_bits)
        return -EINVAL;

    actual_set_bits = aetherfs_bitmap_count_set(bitmap);
    if (aetherfs_le32_to_cpu(bitmap->bb_set_bits) != actual_set_bits)
        return -EBADE;
    if (actual_set_bits != expected_set_bits)
        return -EINVAL;

    checksum = aetherfs_bitmap_checksum(bitmap);
    if (aetherfs_le32_to_cpu(bitmap->bb_checksum) != checksum)
        return -EBADE;

    return 0;
}

static int aetherfs_validate_free_extents(int fd, uint64_t blocks)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_extent_node *node = (struct aetherfs_extent_node *)block;
    uint32_t count;
    uint32_t checksum;
    uint64_t free_blocks = 0;
    uint64_t last_end = AETHERFS_DATA_START_BLOCK;
    int rc;

    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_FREE_EXTENT_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0)
        return rc;

    checksum = aetherfs_extent_node_checksum(node);
    if (aetherfs_le32_to_cpu(node->en_checksum) != checksum)
        return -EBADE;
    if (aetherfs_le64_to_cpu(node->en_next) != 0)
        return -EINVAL;

    count = aetherfs_le32_to_cpu(node->en_count);
    if (count == 0 || count > AETHERFS_EXTENT_LOG_MAX)
        return -EINVAL;

    for (uint32_t index = 0; index < count; ++index) {
        uint64_t start = aetherfs_le64_to_cpu(node->en_extents[index].e_pstart);
        uint64_t len = aetherfs_le64_to_cpu(node->en_extents[index].e_len);

        if (len == 0)
            return -EINVAL;
        if (start < AETHERFS_DATA_START_BLOCK || start < last_end)
            return -EINVAL;
        if (start + len > blocks)
            return -EINVAL;

        last_end = start + len;
        free_blocks += len;
    }

    if (free_blocks != blocks - AETHERFS_DATA_START_BLOCK)
        return -EINVAL;

    return 0;
}

int aetherfs_run_mkfs(const struct aetherfs_mkfs_options *options)
{
    struct aetherfs_format_state state = {.fd = -1};
    int rc;

    if (!options || !options->path)
        return -EINVAL;
    if (options->block_size != AETHERFS_DEF_BLOCK_SIZE)
        return -EOPNOTSUPP;

    rc = aetherfs_prepare_target(options, &state);
    if (rc < 0)
        return rc;

    rc = aetherfs_write_bootstrap(options, &state);
    if (rc < 0)
        goto out;

    rc = aetherfs_validate_super(state.fd, state.blocks);
    if (rc < 0)
        goto out;

    rc = aetherfs_validate_root_inode(state.fd);
    if (rc < 0)
        goto out;

    rc = aetherfs_validate_root_dir(state.fd);
    if (rc < 0)
        goto out;

    rc = aetherfs_validate_journal(state.fd);
    if (rc < 0)
        goto out;

    rc = aetherfs_validate_bitmap(state.fd,
        (uint64_t)AETHERFS_INODE_BITMAP_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        AETHERFS_MAX_INODES, 1);
    if (rc < 0)
        goto out;

    rc = aetherfs_validate_free_extents(state.fd, state.blocks);
    if (rc < 0)
        goto out;

    rc = aetherfs_validate_checkpoint_root(state.fd, state.blocks);
    if (rc < 0)
        goto out;

    printf("formatted %s: blocks=%llu block_size=%u mode=%u\n",
        options->path,
        (unsigned long long)state.blocks,
        options->block_size,
        options->mode);

out:
    if (state.fd >= 0)
        close(state.fd);
    return rc;
}
