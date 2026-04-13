#define _POSIX_C_SOURCE 200809L

#include "rebalance.h"

#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int aetherfs_read_extent_node(struct aetherfs_image *image, uint64_t blocknr,
    struct aetherfs_extent_node *node)
{
    int rc;

    rc = aetherfs_image_read_block(image, blocknr, node);
    if (rc < 0)
        return rc;
    if (aetherfs_le32_to_cpu(node->en_checksum) != aetherfs_extent_node_checksum(node))
        return -EILSEQ;
    if (aetherfs_le64_to_cpu(node->en_next) != 0 ||
        aetherfs_le32_to_cpu(node->en_count) > AETHERFS_EXTENT_LOG_MAX)
        return -EINVAL;
    return 0;
}

static int aetherfs_write_extent_node(struct aetherfs_image *image, uint64_t blocknr,
    struct aetherfs_extent_node *node)
{
    node->en_checksum = aetherfs_cpu_to_le32(aetherfs_extent_node_checksum(node));
    return aetherfs_image_write_block(image, blocknr, node);
}

static int aetherfs_write_checksum_block(struct aetherfs_image *image, uint64_t blocknr,
    struct aetherfs_checksum_block *block)
{
    block->cb_checksum = aetherfs_cpu_to_le32(aetherfs_checksum_block_checksum(block));
    return aetherfs_image_write_block(image, blocknr, block);
}

static int aetherfs_copy_reserved_blocks(const struct aetherfs_image *source, int dst_fd)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];

    for (uint64_t blocknr = 0; blocknr < AETHERFS_DATA_START_BLOCK; ++blocknr) {
        int rc = aetherfs_image_read_block((struct aetherfs_image *)source, blocknr, block);
        if (rc < 0)
            return rc;

        if (pwrite(dst_fd, block, sizeof(block),
                (off_t)(blocknr * (uint64_t)AETHERFS_DEF_BLOCK_SIZE)) !=
            (ssize_t)sizeof(block)) {
            if (errno == EINTR) {
                blocknr--;
                continue;
            }
            return -errno;
        }
    }

    return 0;
}

static int aetherfs_copy_prefix_to_device(const struct aetherfs_image *source, int dst_fd,
    uint64_t blocks)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];

    for (uint64_t blocknr = 0; blocknr < blocks; ++blocknr) {
        int rc = aetherfs_image_read_block((struct aetherfs_image *)source, blocknr, block);
        ssize_t nwritten;

        if (rc < 0)
            return rc;

        nwritten = pwrite(dst_fd, block, sizeof(block),
            (off_t)(blocknr * (uint64_t)AETHERFS_DEF_BLOCK_SIZE));
        if (nwritten != (ssize_t)sizeof(block)) {
            if (nwritten < 0 && errno == EINTR) {
                blocknr--;
                continue;
            }
            return nwritten < 0 ? -errno : -EIO;
        }
    }

    if (fsync(dst_fd) < 0)
        return -errno;

    return 0;
}

static int aetherfs_copy_file_data(struct aetherfs_image *source,
    const struct aetherfs_extent_node *node, struct aetherfs_image *target, uint64_t dst_start)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    uint64_t dst_block = dst_start;

    for (uint32_t extent_index = 0; extent_index < aetherfs_le32_to_cpu(node->en_count);
         ++extent_index) {
        const struct aetherfs_extent *extent = &node->en_extents[extent_index];
        uint64_t pstart = aetherfs_le64_to_cpu(extent->e_pstart);
        uint64_t len = aetherfs_le64_to_cpu(extent->e_len);

        for (uint64_t block_index = 0; block_index < len; ++block_index) {
            int rc = aetherfs_image_read_block(source, pstart + block_index, block);
            if (rc < 0)
                return rc;
            rc = aetherfs_image_write_block(target, dst_block++, block);
            if (rc < 0)
                return rc;
        }
    }

    return 0;
}

static int aetherfs_write_checksum_chain(struct aetherfs_image *target, uint64_t data_start,
    uint64_t data_blocks, uint64_t csum_start)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    uint64_t remaining = data_blocks;
    uint64_t data_block = data_start;
    uint64_t csum_block = csum_start;

    while (remaining) {
        struct aetherfs_checksum_block sums = {0};
        uint32_t entries = (uint32_t)(remaining > AETHERFS_CHECKSUMS_PER_BLOCK ?
            AETHERFS_CHECKSUMS_PER_BLOCK : remaining);
        int rc;

        for (uint32_t index = 0; index < entries; ++index) {
            rc = aetherfs_image_read_block(target, data_block + index, block);
            if (rc < 0)
                return rc;
            sums.cb_sums[index] = aetherfs_cpu_to_le32(aetherfs_crc32c_data(block, sizeof(block)));
        }

        sums.cb_entries = aetherfs_cpu_to_le32(entries);
        sums.cb_next = aetherfs_cpu_to_le64(remaining > entries ? csum_block + 1 : 0);
        rc = aetherfs_write_checksum_block(target, csum_block, &sums);
        if (rc < 0)
            return rc;

        remaining -= entries;
        data_block += entries;
        csum_block++;
    }

    return 0;
}

int aetherfs_run_rebalance(const char *path)
{
    struct aetherfs_image *source = NULL;
    struct aetherfs_image *target = NULL;
    struct aetherfs_image *rewritten = NULL;
    struct aetherfs_scrub_report report = {0};
    char *temp_path = NULL;
    int temp_fd = -1;
    uint64_t next_block = AETHERFS_DATA_START_BLOCK;
    int rc;

    source = calloc(1, sizeof(*source));
    target = calloc(1, sizeof(*target));
    rewritten = calloc(1, sizeof(*rewritten));
    if (!source || !target || !rewritten) {
        rc = -ENOMEM;
        goto out_cleanup;
    }

    rc = aetherfs_image_open(path, false, source);
    if (rc < 0)
        goto out_source;
    if (source->size_bytes == 0) {
        rc = -EOPNOTSUPP;
        goto out_source;
    }

    rc = aetherfs_image_load(source);
    if (rc < 0)
        goto out_source;
    rc = aetherfs_image_validate(source, &report);
    if (rc < 0)
        goto out_source;

    rc = aetherfs_image_make_temp_path(path, source->size_bytes, &temp_path);
    if (rc < 0)
        goto out_source;

    temp_fd = open(temp_path, O_RDWR | O_TRUNC | O_CLOEXEC);
    if (temp_fd < 0) {
        rc = -errno;
        goto out_source;
    }
    if (ftruncate(temp_fd, (off_t)source->size_bytes) < 0) {
        rc = -errno;
        goto out_source;
    }

    rc = aetherfs_copy_reserved_blocks(source, temp_fd);
    if (rc < 0)
        goto out_source;
    close(temp_fd);
    temp_fd = -1;

    rc = aetherfs_image_open(temp_path, true, target);
    if (rc < 0)
        goto out_source;
    rc = aetherfs_image_load(target);
    if (rc < 0)
        goto out_target;

    memset(&target->free_extents, 0, sizeof(target->free_extents));

    for (uint32_t index = 1; index < AETHERFS_MAX_INODES; ++index) {
        struct aetherfs_inode *inode = &target->inode_table[index];
        const struct aetherfs_inode *source_inode = &source->inode_table[index];
        struct aetherfs_extent_node source_extents;
        struct aetherfs_extent_node new_extents = {0};
        uint16_t mode;
        uint64_t file_blocks;
        uint64_t checksum_blocks;
        uint64_t data_start;
        uint64_t csum_start;
        uint64_t extent_root;

        if (!aetherfs_image_bitmap_test(&target->inode_bitmap, index))
            continue;

        mode = aetherfs_le16_to_cpu(inode->i_mode);
        if (!S_ISREG(mode))
            continue;

        file_blocks = aetherfs_le64_to_cpu(source_inode->i_blocks);
        if (!file_blocks) {
            inode->i_extent_root = aetherfs_cpu_to_le64(0);
            inode->i_checksum = aetherfs_cpu_to_le32(aetherfs_inode_checksum(inode));
            continue;
        }

        rc = aetherfs_read_extent_node(source,
            aetherfs_le64_to_cpu(source_inode->i_extent_root), &source_extents);
        if (rc < 0)
            goto out_target;

        extent_root = next_block++;
        data_start = next_block;
        next_block += file_blocks;
        checksum_blocks = (file_blocks + AETHERFS_CHECKSUMS_PER_BLOCK - 1) /
            AETHERFS_CHECKSUMS_PER_BLOCK;
        csum_start = next_block;
        next_block += checksum_blocks;

        if (next_block > target->blocks) {
            rc = -ENOSPC;
            goto out_target;
        }

        rc = aetherfs_copy_file_data(source, &source_extents, target, data_start);
        if (rc < 0)
            goto out_target;
        rc = aetherfs_write_checksum_chain(target, data_start, file_blocks, csum_start);
        if (rc < 0)
            goto out_target;

        new_extents.en_count = aetherfs_cpu_to_le32(1);
        new_extents.en_extents[0].e_lstart = aetherfs_cpu_to_le64(0);
        new_extents.en_extents[0].e_pstart = aetherfs_cpu_to_le64(data_start);
        new_extents.en_extents[0].e_csum_start = aetherfs_cpu_to_le64(csum_start);
        new_extents.en_extents[0].e_len = aetherfs_cpu_to_le64(file_blocks);
        rc = aetherfs_write_extent_node(target, extent_root, &new_extents);
        if (rc < 0)
            goto out_target;

        inode->i_extent_root = aetherfs_cpu_to_le64(extent_root);
        inode->i_checksum = aetherfs_cpu_to_le32(aetherfs_inode_checksum(inode));
    }

    if (next_block < target->blocks) {
        target->free_extents.en_count = aetherfs_cpu_to_le32(1);
        target->free_extents.en_extents[0].e_pstart = aetherfs_cpu_to_le64(next_block);
        target->free_extents.en_extents[0].e_len =
            aetherfs_cpu_to_le64(target->blocks - next_block);
    } else {
        target->free_extents.en_count = aetherfs_cpu_to_le32(0);
    }

    target->super.s_free_blocks = aetherfs_cpu_to_le64(target->blocks - next_block);
    target->super.s_checksum = aetherfs_cpu_to_le32(aetherfs_super_checksum(&target->super));
    target->journal.il_outstanding = aetherfs_cpu_to_le32(0);
    target->journal.il_checksum = aetherfs_cpu_to_le32(aetherfs_journal_checksum(&target->journal));

    rc = aetherfs_image_write_inode_table(target);
    if (rc < 0)
        goto out_target;
    rc = aetherfs_image_write_free_extents(target);
    if (rc < 0)
        goto out_target;
    rc = aetherfs_image_write_super(target);
    if (rc < 0)
        goto out_target;
    rc = aetherfs_image_write_journal(target);
    if (rc < 0)
        goto out_target;

    rc = aetherfs_image_load(target);
    if (rc < 0)
        goto out_target;
    rc = aetherfs_image_validate(target, &report);
    if (rc < 0)
        goto out_target;

    if (source->is_block_device) {
        int dst_fd;

        dst_fd = open(path, O_RDWR | O_CLOEXEC);
        if (dst_fd < 0) {
            rc = -errno;
            goto out_target;
        }

        rc = aetherfs_copy_prefix_to_device(target, dst_fd, next_block);
        close(dst_fd);
        if (rc < 0)
            goto out_target;

        aetherfs_image_close(target);

        rc = aetherfs_image_open(path, false, rewritten);
        if (rc < 0)
            goto out_source;
        rc = aetherfs_image_load(rewritten);
        if (rc < 0) {
            aetherfs_image_close(rewritten);
            goto out_source;
        }
        rc = aetherfs_image_validate(rewritten, &report);
        aetherfs_image_close(rewritten);
        if (rc < 0)
            goto out_source;
    } else {
        aetherfs_image_close(target);
        aetherfs_image_close(source);

        if (rename(temp_path, path) < 0) {
            rc = -errno;
            goto out_cleanup;
        }
    }

    aetherfs_image_close(source);

    printf("rebalance ok: files=%u data_blocks=%llu free_blocks=%llu\n",
        report.files,
        (unsigned long long)report.data_blocks,
        (unsigned long long)report.free_blocks);

    unlink(temp_path);
    free(temp_path);
    return 0;

out_target:
    aetherfs_image_close(target);
out_source:
    aetherfs_image_close(source);
    if (temp_fd >= 0)
        close(temp_fd);
out_cleanup:
    if (rc < 0 && temp_path)
        unlink(temp_path);
    free(temp_path);
    free(rewritten);
    free(target);
    free(source);
    return rc;
}
