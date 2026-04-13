#define _POSIX_C_SOURCE 200809L

#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <linux/fs.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

struct aetherfs_interval {
    uint64_t start;
    uint64_t len;
};

struct aetherfs_interval_list {
    struct aetherfs_interval *items;
    size_t count;
    size_t capacity;
};

struct aetherfs_pool_mapping {
    uint32_t member_index;
    uint32_t parity_member_index;
    uint64_t member_block;
};

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

static uint8_t aetherfs_dirent_checksum(const struct aetherfs_dir_entry *entry)
{
    uint8_t scratch[AETHERFS_DIR_ENTRY_BASE_SIZE + AETHERFS_MAX_NAMELEN] = {0};
    uint16_t name_len = aetherfs_le16_to_cpu(entry->name_len);
    size_t entry_len = AETHERFS_DIR_ENTRY_BASE_SIZE + name_len;
    uint32_t crc;

    if (name_len > AETHERFS_MAX_NAMELEN)
        return 0;

    memcpy(scratch, entry, entry_len);
    scratch[offsetof(struct aetherfs_dir_entry, checksum)] = 0;
    crc = aetherfs_crc32c_data(scratch, entry_len);
    return (uint8_t)(crc & 0xFFU);
}

static void aetherfs_interval_list_reset(struct aetherfs_interval_list *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int aetherfs_interval_list_append(struct aetherfs_interval_list *list,
    uint64_t start, uint64_t len)
{
    struct aetherfs_interval *items;
    size_t new_capacity;

    if (!len)
        return 0;

    if (list->count == list->capacity) {
        new_capacity = list->capacity ? list->capacity * 2 : 16;
        items = realloc(list->items, new_capacity * sizeof(*items));
        if (!items)
            return -ENOMEM;
        list->items = items;
        list->capacity = new_capacity;
    }

    list->items[list->count].start = start;
    list->items[list->count].len = len;
    list->count++;
    return 0;
}

static int aetherfs_interval_compare(const void *left, const void *right)
{
    const struct aetherfs_interval *lhs = left;
    const struct aetherfs_interval *rhs = right;

    if (lhs->start < rhs->start)
        return -1;
    if (lhs->start > rhs->start)
        return 1;
    if (lhs->len < rhs->len)
        return -1;
    if (lhs->len > rhs->len)
        return 1;
    return 0;
}

static int aetherfs_mask_first(uint32_t mask)
{
    for (uint32_t index = 0; index < AETHERFS_MAX_DEVICES; ++index) {
        if (mask & (1U << index))
            return (int)index;
    }
    return -1;
}

static uint32_t aetherfs_mask_count(uint32_t mask)
{
    return (uint32_t)__builtin_popcount(mask);
}

static int aetherfs_mask_nth(uint32_t mask, uint32_t ordinal)
{
    for (uint32_t index = 0; index < AETHERFS_MAX_DEVICES; ++index) {
        if (!(mask & (1U << index)))
            continue;
        if (ordinal == 0)
            return (int)index;
        ordinal--;
    }
    return -1;
}

static uint32_t aetherfs_pool_member_limit_mask(const struct aetherfs_image *image)
{
    if (image->member_count >= 32)
        return UINT32_MAX;
    return image->member_count == 0 ? 0U : ((1U << image->member_count) - 1U);
}

static uint32_t aetherfs_pool_profile(const struct aetherfs_image *image)
{
    return aetherfs_le32_to_cpu(image->pool_label.pl_profile);
}

static uint32_t aetherfs_pool_metadata_mask(const struct aetherfs_image *image)
{
    uint32_t mask = aetherfs_le32_to_cpu(image->pool_label.pl_metadata_mask);

    mask &= aetherfs_pool_member_limit_mask(image);
    if (mask == 0 && image->member_count > 0)
        mask = 1U;
    return mask;
}

static uint32_t aetherfs_pool_data_mask(const struct aetherfs_image *image)
{
    uint32_t mask = aetherfs_le32_to_cpu(image->pool_label.pl_data_mask);

    mask &= aetherfs_pool_member_limit_mask(image);
    if (mask == 0 && image->member_count > 0)
        mask = 1U;
    return mask;
}

static uint32_t aetherfs_pool_log_mask(const struct aetherfs_image *image)
{
    uint32_t mask = aetherfs_le32_to_cpu(image->pool_label.pl_log_mask);

    mask &= aetherfs_pool_member_limit_mask(image);
    if (mask == 0)
        mask = aetherfs_pool_metadata_mask(image);
    return mask;
}

static uint64_t aetherfs_pool_member_total_blocks(const struct aetherfs_image *image, uint32_t index)
{
    uint64_t total = aetherfs_le64_to_cpu(image->pool_label.pl_members[index].pm_total_blocks);

    if (total == 0)
        total = image->members[index].blocks;
    return total;
}

static uint64_t aetherfs_pool_member_data_blocks(const struct aetherfs_image *image, uint32_t index)
{
    uint64_t blocks = aetherfs_le64_to_cpu(image->pool_label.pl_members[index].pm_data_blocks);

    if (blocks == 0 && aetherfs_pool_member_total_blocks(image, index) > AETHERFS_DATA_START_BLOCK)
        blocks = aetherfs_pool_member_total_blocks(image, index) - AETHERFS_DATA_START_BLOCK;
    return blocks;
}

static int aetherfs_member_open_path(const char *path, bool writable,
    struct aetherfs_image_member *member)
{
    struct stat st;
    int flags = writable ? O_RDWR : O_RDONLY;
    int fd;

    if (!path || !member)
        return -EINVAL;

    memset(member, 0, sizeof(*member));
    member->fd = -1;

    fd = open(path, flags | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    if (fstat(fd, &st) < 0) {
        close(fd);
        return -errno;
    }
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
        close(fd);
        return -EINVAL;
    }

    member->fd = fd;
    if (S_ISBLK(st.st_mode)) {
        uint64_t device_bytes = 0;

        if (ioctl(fd, BLKGETSIZE64, &device_bytes) < 0) {
            close(fd);
            return -errno;
        }
        member->size_bytes = device_bytes;
        member->is_block_device = true;
    } else {
        member->size_bytes = (uint64_t)st.st_size;
    }

    member->blocks = member->size_bytes / (uint64_t)AETHERFS_DEF_BLOCK_SIZE;
    return 0;
}

static int aetherfs_member_read_block(const struct aetherfs_image_member *member, uint64_t block,
    void *buf)
{
    if (!member || member->fd < 0)
        return -EINVAL;
    return aetherfs_read_all(member->fd, block * AETHERFS_DEF_BLOCK_SIZE, buf,
        AETHERFS_DEF_BLOCK_SIZE);
}

static int aetherfs_member_write_block(const struct aetherfs_image_member *member, uint64_t block,
    const void *buf)
{
    if (!member || member->fd < 0)
        return -EINVAL;
    return aetherfs_write_all(member->fd, block * AETHERFS_DEF_BLOCK_SIZE, buf,
        AETHERFS_DEF_BLOCK_SIZE);
}

static int aetherfs_pool_label_validate(const struct aetherfs_pool_label *label)
{
    uint32_t count;
    uint32_t profile;
    uint32_t stripe_blocks;
    uint32_t limit_mask;
    uint64_t logical_blocks;
    uint64_t logical_data_blocks;

    if (aetherfs_le32_to_cpu(label->pl_magic) != AETHERFS_POOL_MAGIC)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(label->pl_version) != AETHERFS_POOL_VERSION)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(label->pl_checksum) != aetherfs_pool_label_checksum(label))
        return -EILSEQ;

    count = aetherfs_le32_to_cpu(label->pl_member_count);
    profile = aetherfs_le32_to_cpu(label->pl_profile);
    stripe_blocks = aetherfs_le32_to_cpu(label->pl_stripe_blocks);
    logical_blocks = aetherfs_le64_to_cpu(label->pl_logical_blocks);
    logical_data_blocks = aetherfs_le64_to_cpu(label->pl_logical_data_blocks);

    if (count == 0 || count > AETHERFS_MAX_DEVICES)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(label->pl_member_index) >= count)
        return -EINVAL;
    if (profile > AETHERFS_POOL_PROFILE_TIERED)
        return -EINVAL;
    if (stripe_blocks == 0)
        return -EINVAL;
    if (logical_blocks < AETHERFS_DATA_START_BLOCK)
        return -EINVAL;
    if (logical_blocks != logical_data_blocks + AETHERFS_DATA_START_BLOCK)
        return -EINVAL;

    limit_mask = (1U << count) - 1U;
    if ((aetherfs_le32_to_cpu(label->pl_metadata_mask) & ~limit_mask) != 0 ||
        (aetherfs_le32_to_cpu(label->pl_data_mask) & ~limit_mask) != 0 ||
        (aetherfs_le32_to_cpu(label->pl_log_mask) & ~limit_mask) != 0 ||
        (aetherfs_le32_to_cpu(label->pl_cache_mask) & ~limit_mask) != 0) {
        return -EINVAL;
    }

    for (uint32_t index = 0; index < count; ++index) {
        const struct aetherfs_pool_member *member = &label->pl_members[index];
        uint64_t total_blocks = aetherfs_le64_to_cpu(member->pm_total_blocks);
        uint64_t data_blocks = aetherfs_le64_to_cpu(member->pm_data_blocks);

        if (aetherfs_le32_to_cpu(member->pm_index) != index)
            return -EINVAL;
        if (total_blocks < AETHERFS_BOOTSTRAP_BLOCKS)
            return -EINVAL;
        if (data_blocks > total_blocks - AETHERFS_DATA_START_BLOCK)
            return -EINVAL;
    }

    return 0;
}

static int aetherfs_member_read_pool_label(const struct aetherfs_image_member *member,
    struct aetherfs_pool_label *label, bool *present)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    int rc;

    *present = false;
    rc = aetherfs_member_read_block(member, AETHERFS_POOL_LABEL_BLOCK, block);
    if (rc < 0)
        return rc;

    memcpy(label, block, sizeof(*label));
    if (aetherfs_le32_to_cpu(label->pl_magic) != AETHERFS_POOL_MAGIC)
        return 0;

    rc = aetherfs_pool_label_validate(label);
    if (rc < 0)
        return rc;

    *present = true;
    return 0;
}

static void aetherfs_pool_label_normalize(struct aetherfs_pool_label *label)
{
    label->pl_checksum = 0;
    label->pl_member_index = 0;
}

static int aetherfs_pool_labels_equivalent(const struct aetherfs_pool_label *left,
    const struct aetherfs_pool_label *right)
{
    struct aetherfs_pool_label lhs;
    struct aetherfs_pool_label rhs;

    memcpy(&lhs, left, sizeof(lhs));
    memcpy(&rhs, right, sizeof(rhs));
    aetherfs_pool_label_normalize(&lhs);
    aetherfs_pool_label_normalize(&rhs);
    return memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
}

static void aetherfs_image_assign_primary_aliases(struct aetherfs_image *image)
{
    int primary = aetherfs_mask_first(aetherfs_pool_metadata_mask(image));

    if (primary < 0)
        primary = 0;
    image->fd = image->members[primary].fd;
    image->is_block_device = image->member_count == 1 ? image->members[primary].is_block_device : false;
    image->size_bytes = image->member_count == 1
        ? image->members[primary].size_bytes
        : aetherfs_le64_to_cpu(image->pool_label.pl_logical_blocks) * (uint64_t)AETHERFS_DEF_BLOCK_SIZE;
}

static void aetherfs_image_synthesize_single_label(struct aetherfs_image *image)
{
    struct aetherfs_pool_label *label = &image->pool_label;
    uint64_t blocks = image->members[0].blocks;

    memset(label, 0, sizeof(*label));
    label->pl_magic = aetherfs_cpu_to_le32(AETHERFS_POOL_MAGIC);
    label->pl_version = aetherfs_cpu_to_le32(AETHERFS_POOL_VERSION);
    label->pl_profile = aetherfs_cpu_to_le32(AETHERFS_POOL_PROFILE_SINGLE);
    label->pl_member_count = aetherfs_cpu_to_le32(1);
    label->pl_member_index = aetherfs_cpu_to_le32(0);
    label->pl_stripe_blocks = aetherfs_cpu_to_le32(1);
    label->pl_metadata_mask = aetherfs_cpu_to_le32(1);
    label->pl_data_mask = aetherfs_cpu_to_le32(1);
    label->pl_log_mask = aetherfs_cpu_to_le32(1);
    label->pl_logical_blocks = aetherfs_cpu_to_le64(blocks);
    label->pl_logical_data_blocks = aetherfs_cpu_to_le64(blocks - AETHERFS_DATA_START_BLOCK);
    label->pl_members[0].pm_total_blocks = aetherfs_cpu_to_le64(blocks);
    label->pl_members[0].pm_data_blocks = aetherfs_cpu_to_le64(blocks - AETHERFS_DATA_START_BLOCK);
    label->pl_members[0].pm_role_mask = aetherfs_cpu_to_le32(
        AETHERFS_POOL_MEMBER_DATA | AETHERFS_POOL_MEMBER_METADATA | AETHERFS_POOL_MEMBER_LOG);
    label->pl_checksum = aetherfs_cpu_to_le32(aetherfs_pool_label_checksum(label));

    image->member_count = 1;
    image->members[0].role_mask = AETHERFS_POOL_MEMBER_DATA |
        AETHERFS_POOL_MEMBER_METADATA | AETHERFS_POOL_MEMBER_LOG;
    image->blocks = blocks;
    aetherfs_image_assign_primary_aliases(image);
}

static int aetherfs_image_read_physical_member(const struct aetherfs_image *image, uint32_t member_index,
    uint64_t block, void *buf)
{
    if (!image || member_index >= image->member_count)
        return -EINVAL;
    return aetherfs_member_read_block(&image->members[member_index], block, buf);
}

static int aetherfs_image_write_physical_member(struct aetherfs_image *image, uint32_t member_index,
    uint64_t block, const void *buf)
{
    if (!image || member_index >= image->member_count)
        return -EINVAL;
    return aetherfs_member_write_block(&image->members[member_index], block, buf);
}

static uint32_t aetherfs_pool_role_mask_for_bootstrap_block(const struct aetherfs_image *image,
    uint64_t block)
{
    if (block == AETHERFS_JOURNAL_BLOCK)
        return aetherfs_pool_log_mask(image);
    return aetherfs_pool_metadata_mask(image);
}

static int aetherfs_pool_map_linear(const struct aetherfs_image *image, uint64_t logical_block,
    struct aetherfs_pool_mapping *mapping)
{
    uint32_t mask = aetherfs_pool_data_mask(image);
    uint64_t remaining = logical_block - AETHERFS_DATA_START_BLOCK;

    memset(mapping, 0, sizeof(*mapping));
    mapping->parity_member_index = UINT32_MAX;

    for (uint32_t ordinal = 0; ordinal < aetherfs_mask_count(mask); ++ordinal) {
        int member_index = aetherfs_mask_nth(mask, ordinal);
        uint64_t blocks;

        if (member_index < 0)
            break;
        blocks = aetherfs_pool_member_data_blocks(image, (uint32_t)member_index);
        if (remaining < blocks) {
            mapping->member_index = (uint32_t)member_index;
            mapping->member_block = AETHERFS_DATA_START_BLOCK + remaining;
            return 0;
        }
        remaining -= blocks;
    }

    return -ERANGE;
}

static int aetherfs_pool_map_mirror(const struct aetherfs_image *image, uint64_t logical_block,
    struct aetherfs_pool_mapping *mapping)
{
    uint32_t mask = aetherfs_pool_data_mask(image);
    int member_index = aetherfs_mask_first(mask);

    if (member_index < 0)
        return -EINVAL;

    memset(mapping, 0, sizeof(*mapping));
    mapping->member_index = (uint32_t)member_index;
    mapping->parity_member_index = UINT32_MAX;
    mapping->member_block = logical_block;
    return 0;
}

static int aetherfs_pool_map_stripe(const struct aetherfs_image *image, uint64_t logical_block,
    struct aetherfs_pool_mapping *mapping)
{
    uint32_t mask = aetherfs_pool_data_mask(image);
    uint32_t data_members = aetherfs_mask_count(mask);
    uint32_t stripe_blocks = aetherfs_le32_to_cpu(image->pool_label.pl_stripe_blocks);
    uint64_t relative = logical_block - AETHERFS_DATA_START_BLOCK;
    uint64_t stripe_number;
    uint64_t member_stripe;
    uint64_t within_stripe;
    uint32_t member_ordinal;
    int member_index;

    if (data_members == 0 || stripe_blocks == 0)
        return -EINVAL;

    stripe_number = relative / stripe_blocks;
    member_ordinal = (uint32_t)(stripe_number % data_members);
    member_stripe = stripe_number / data_members;
    within_stripe = relative % stripe_blocks;
    member_index = aetherfs_mask_nth(mask, member_ordinal);
    if (member_index < 0)
        return -EINVAL;

    memset(mapping, 0, sizeof(*mapping));
    mapping->member_index = (uint32_t)member_index;
    mapping->parity_member_index = UINT32_MAX;
    mapping->member_block = AETHERFS_DATA_START_BLOCK +
        member_stripe * stripe_blocks + within_stripe;
    if (mapping->member_block >=
        AETHERFS_DATA_START_BLOCK + aetherfs_pool_member_data_blocks(image, mapping->member_index)) {
        return -ERANGE;
    }
    return 0;
}

static int aetherfs_pool_map_parity(const struct aetherfs_image *image, uint64_t logical_block,
    struct aetherfs_pool_mapping *mapping)
{
    uint32_t mask = aetherfs_pool_data_mask(image);
    uint32_t data_members = aetherfs_mask_count(mask);
    uint32_t stripe_blocks = aetherfs_le32_to_cpu(image->pool_label.pl_stripe_blocks);
    uint64_t relative = logical_block - AETHERFS_DATA_START_BLOCK;
    uint64_t row;
    uint64_t position;
    uint64_t within_stripe;
    uint32_t parity_ordinal;
    uint32_t data_ordinal;
    uint32_t actual_ordinal;
    int member_index;
    int parity_member_index;

    if (data_members < 3 || stripe_blocks == 0)
        return -EINVAL;

    row = relative / ((uint64_t)(data_members - 1U) * stripe_blocks);
    position = relative % ((uint64_t)(data_members - 1U) * stripe_blocks);
    parity_ordinal = (uint32_t)(row % data_members);
    data_ordinal = (uint32_t)(position / stripe_blocks);
    actual_ordinal = data_ordinal >= parity_ordinal ? data_ordinal + 1U : data_ordinal;
    within_stripe = position % stripe_blocks;

    member_index = aetherfs_mask_nth(mask, actual_ordinal);
    parity_member_index = aetherfs_mask_nth(mask, parity_ordinal);
    if (member_index < 0 || parity_member_index < 0)
        return -EINVAL;

    memset(mapping, 0, sizeof(*mapping));
    mapping->member_index = (uint32_t)member_index;
    mapping->parity_member_index = (uint32_t)parity_member_index;
    mapping->member_block = AETHERFS_DATA_START_BLOCK + row * stripe_blocks + within_stripe;
    if (mapping->member_block >=
        AETHERFS_DATA_START_BLOCK + aetherfs_pool_member_data_blocks(image, mapping->member_index) ||
        mapping->member_block >=
        AETHERFS_DATA_START_BLOCK + aetherfs_pool_member_data_blocks(image, mapping->parity_member_index)) {
        return -ERANGE;
    }
    return 0;
}

static int aetherfs_pool_map_data_block(const struct aetherfs_image *image, uint64_t logical_block,
    struct aetherfs_pool_mapping *mapping)
{
    switch (aetherfs_pool_profile(image)) {
    case AETHERFS_POOL_PROFILE_SINGLE:
    case AETHERFS_POOL_PROFILE_TIERED:
        return aetherfs_pool_map_linear(image, logical_block, mapping);
    case AETHERFS_POOL_PROFILE_MIRROR:
        return aetherfs_pool_map_mirror(image, logical_block, mapping);
    case AETHERFS_POOL_PROFILE_STRIPE:
        return aetherfs_pool_map_stripe(image, logical_block, mapping);
    case AETHERFS_POOL_PROFILE_PARITY:
        return aetherfs_pool_map_parity(image, logical_block, mapping);
    default:
        return -EINVAL;
    }
}

static int aetherfs_read_extent_node(struct aetherfs_image *image, uint64_t blocknr,
    struct aetherfs_extent_node *node)
{
    int rc;

    rc = aetherfs_image_read_block(image, blocknr, node);
    if (rc < 0)
        return rc;
    if (aetherfs_le32_to_cpu(node->en_checksum) != aetherfs_extent_node_checksum(node))
        return -EILSEQ;
    if (aetherfs_le32_to_cpu(node->en_count) > AETHERFS_EXTENT_LOG_MAX)
        return -EINVAL;
    return 0;
}

static int aetherfs_write_extent_node(struct aetherfs_image *image, uint64_t blocknr,
    struct aetherfs_extent_node *node)
{
    node->en_checksum = aetherfs_cpu_to_le32(aetherfs_extent_node_checksum(node));
    return aetherfs_image_write_block(image, blocknr, node);
}

static int aetherfs_read_checksum_block(struct aetherfs_image *image, uint64_t blocknr,
    struct aetherfs_checksum_block *block)
{
    int rc;

    rc = aetherfs_image_read_block(image, blocknr, block);
    if (rc < 0)
        return rc;
    if (aetherfs_le32_to_cpu(block->cb_checksum) != aetherfs_checksum_block_checksum(block))
        return -EILSEQ;
    if (aetherfs_le32_to_cpu(block->cb_entries) > AETHERFS_CHECKSUMS_PER_BLOCK)
        return -EINVAL;
    return 0;
}

static int aetherfs_write_checksum_block(struct aetherfs_image *image, uint64_t blocknr,
    struct aetherfs_checksum_block *block)
{
    block->cb_checksum = aetherfs_cpu_to_le32(aetherfs_checksum_block_checksum(block));
    return aetherfs_image_write_block(image, blocknr, block);
}

uint32_t aetherfs_image_bitmap_count_set(const struct aetherfs_bitmap_block *bitmap)
{
    uint32_t count = 0;

    for (size_t index = 0; index < sizeof(bitmap->bb_bits); ++index)
        count += (uint32_t)__builtin_popcount((unsigned int)bitmap->bb_bits[index]);

    return count;
}

bool aetherfs_image_bitmap_test(const struct aetherfs_bitmap_block *bitmap, uint32_t bit)
{
    return (bitmap->bb_bits[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0;
}

void aetherfs_image_bitmap_set(struct aetherfs_bitmap_block *bitmap, uint32_t bit)
{
    bitmap->bb_bits[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
}

void aetherfs_image_bitmap_clear(struct aetherfs_bitmap_block *bitmap, uint32_t bit)
{
    bitmap->bb_bits[bit / 8U] &= (uint8_t)~(1U << (bit % 8U));
}

int aetherfs_image_open(const char *path, bool writable, struct aetherfs_image *image)
{
    struct aetherfs_pool_label label;
    bool present = false;
    int rc;

    if (!path || !image)
        return -EINVAL;

    memset(image, 0, sizeof(*image));
    image->fd = -1;
    image->member_count = 1;

    rc = aetherfs_member_open_path(path, writable, &image->members[0]);
    if (rc < 0)
        return rc;

    image->writable = writable;
    rc = aetherfs_member_read_pool_label(&image->members[0], &label, &present);
    if (rc < 0) {
        aetherfs_image_close(image);
        return rc;
    }

    if (present) {
        if (aetherfs_le32_to_cpu(label.pl_member_count) != 1) {
            aetherfs_image_close(image);
            return -EXDEV;
        }
        memcpy(&image->pool_label, &label, sizeof(label));
        image->members[0].class_mask = aetherfs_le32_to_cpu(label.pl_members[0].pm_class_mask);
        image->members[0].role_mask = aetherfs_le32_to_cpu(label.pl_members[0].pm_role_mask);
        image->blocks = aetherfs_le64_to_cpu(label.pl_logical_blocks);
        image->member_count = 1;
        aetherfs_image_assign_primary_aliases(image);
    } else {
        aetherfs_image_synthesize_single_label(image);
    }

    return 0;
}

int aetherfs_image_open_pool(const struct aetherfs_device_spec *members, uint32_t member_count,
    bool writable, struct aetherfs_image *image)
{
    struct aetherfs_pool_label labels[AETHERFS_MAX_DEVICES];
    struct aetherfs_pool_label base_label;
    bool present[AETHERFS_MAX_DEVICES] = {0};
    bool seen_indices[AETHERFS_MAX_DEVICES] = {0};
    struct aetherfs_image_member opened[AETHERFS_MAX_DEVICES];
    uint32_t expected_count;
    int rc;

    if (!members || !image || member_count == 0 || member_count > AETHERFS_MAX_DEVICES)
        return -EINVAL;
    if (member_count == 1)
        return aetherfs_image_open(members[0].path, writable, image);

    memset(image, 0, sizeof(*image));
    image->fd = -1;
    image->writable = writable;

    for (uint32_t index = 0; index < member_count; ++index) {
        opened[index].fd = -1;
        rc = aetherfs_member_open_path(members[index].path, writable, &opened[index]);
        if (rc < 0)
            goto fail;
        rc = aetherfs_member_read_pool_label(&opened[index], &labels[index], &present[index]);
        if (rc < 0)
            goto fail;
        if (!present[index]) {
            rc = -EINVAL;
            goto fail;
        }
    }

    memcpy(&base_label, &labels[0], sizeof(base_label));
    expected_count = aetherfs_le32_to_cpu(base_label.pl_member_count);
    if (expected_count != member_count) {
        rc = -EINVAL;
        goto fail;
    }

    for (uint32_t index = 0; index < member_count; ++index) {
        uint32_t member_index = aetherfs_le32_to_cpu(labels[index].pl_member_index);

        if (!aetherfs_pool_labels_equivalent(&base_label, &labels[index])) {
            rc = -EINVAL;
            goto fail;
        }
        if (member_index >= member_count || seen_indices[member_index]) {
            rc = -EINVAL;
            goto fail;
        }
        seen_indices[member_index] = true;
        image->members[member_index] = opened[index];
        image->members[member_index].class_mask =
            aetherfs_le32_to_cpu(labels[index].pl_members[member_index].pm_class_mask);
        image->members[member_index].role_mask =
            aetherfs_le32_to_cpu(labels[index].pl_members[member_index].pm_role_mask);
        image->members[member_index].member_index = member_index;
        image->members[member_index].blocks =
            aetherfs_le64_to_cpu(labels[index].pl_members[member_index].pm_total_blocks);
        image->members[member_index].size_bytes =
            image->members[member_index].blocks * (uint64_t)AETHERFS_DEF_BLOCK_SIZE;
        opened[index].fd = -1;
    }

    for (uint32_t index = 0; index < member_count; ++index) {
        if (!seen_indices[index]) {
            rc = -EINVAL;
            goto fail;
        }
    }

    base_label.pl_member_index = aetherfs_cpu_to_le32(0);
    base_label.pl_checksum = aetherfs_cpu_to_le32(aetherfs_pool_label_checksum(&base_label));
    image->pool_label = base_label;
    image->member_count = member_count;
    image->blocks = aetherfs_le64_to_cpu(image->pool_label.pl_logical_blocks);
    aetherfs_image_assign_primary_aliases(image);
    return 0;

fail:
    for (uint32_t index = 0; index < member_count; ++index) {
        if (opened[index].fd >= 0)
            close(opened[index].fd);
    }
    aetherfs_image_close(image);
    return rc;
}

void aetherfs_image_close(struct aetherfs_image *image)
{
    if (!image)
        return;

    for (uint32_t index = 0; index < image->member_count; ++index) {
        if (image->members[index].fd >= 0) {
            close(image->members[index].fd);
            image->members[index].fd = -1;
        }
    }

    image->fd = -1;
}

int aetherfs_image_read_block(struct aetherfs_image *image, uint64_t block, void *buf)
{
    struct aetherfs_pool_mapping mapping;
    uint32_t mask;
    int rc;

    if (!image || !buf)
        return -EINVAL;

    if (block < AETHERFS_DATA_START_BLOCK) {
        mask = aetherfs_pool_role_mask_for_bootstrap_block(image, block);
        for (uint32_t ordinal = 0; ordinal < aetherfs_mask_count(mask); ++ordinal) {
            int member_index = aetherfs_mask_nth(mask, ordinal);

            if (member_index < 0)
                break;
            rc = aetherfs_image_read_physical_member(image, (uint32_t)member_index, block, buf);
            if (rc == 0)
                return 0;
        }
        return -EIO;
    }

    rc = aetherfs_pool_map_data_block(image, block, &mapping);
    if (rc < 0)
        return rc;
    return aetherfs_image_read_physical_member(image, mapping.member_index, mapping.member_block, buf);
}

int aetherfs_image_write_block(struct aetherfs_image *image, uint64_t block, const void *buf)
{
    struct aetherfs_pool_mapping mapping;
    uint32_t mask;
    int rc;

    if (!image || !buf)
        return -EINVAL;
    if (!image->writable)
        return -EBADF;

    if (block < AETHERFS_DATA_START_BLOCK) {
        mask = aetherfs_pool_role_mask_for_bootstrap_block(image, block);
        if (mask == 0)
            return -EINVAL;
        for (uint32_t ordinal = 0; ordinal < aetherfs_mask_count(mask); ++ordinal) {
            int member_index = aetherfs_mask_nth(mask, ordinal);

            if (member_index < 0)
                break;
            rc = aetherfs_image_write_physical_member(image, (uint32_t)member_index, block, buf);
            if (rc < 0)
                return rc;
        }
        return 0;
    }

    rc = aetherfs_pool_map_data_block(image, block, &mapping);
    if (rc < 0)
        return rc;

    switch (aetherfs_pool_profile(image)) {
    case AETHERFS_POOL_PROFILE_SINGLE:
    case AETHERFS_POOL_PROFILE_TIERED:
    case AETHERFS_POOL_PROFILE_STRIPE:
        return aetherfs_image_write_physical_member(image, mapping.member_index,
            mapping.member_block, buf);
    case AETHERFS_POOL_PROFILE_MIRROR:
        mask = aetherfs_pool_data_mask(image);
        for (uint32_t ordinal = 0; ordinal < aetherfs_mask_count(mask); ++ordinal) {
            int member_index = aetherfs_mask_nth(mask, ordinal);

            if (member_index < 0)
                break;
            rc = aetherfs_image_write_physical_member(image, (uint32_t)member_index,
                mapping.member_block, buf);
            if (rc < 0)
                return rc;
        }
        return 0;
    case AETHERFS_POOL_PROFILE_PARITY:
    {
        uint8_t old_data[AETHERFS_DEF_BLOCK_SIZE];
        uint8_t old_parity[AETHERFS_DEF_BLOCK_SIZE];
        uint8_t new_parity[AETHERFS_DEF_BLOCK_SIZE];

        rc = aetherfs_image_read_physical_member(image, mapping.member_index,
            mapping.member_block, old_data);
        if (rc < 0)
            return rc;
        rc = aetherfs_image_read_physical_member(image, mapping.parity_member_index,
            mapping.member_block, old_parity);
        if (rc < 0)
            return rc;

        for (size_t index = 0; index < sizeof(new_parity); ++index) {
            new_parity[index] = (uint8_t)(old_parity[index] ^ old_data[index] ^
                ((const uint8_t *)buf)[index]);
        }

        rc = aetherfs_image_write_physical_member(image, mapping.parity_member_index,
            mapping.member_block, new_parity);
        if (rc < 0)
            return rc;
        return aetherfs_image_write_physical_member(image, mapping.member_index,
            mapping.member_block, buf);
    }
    default:
        return -EINVAL;
    }
}

static int aetherfs_image_validate_super(const struct aetherfs_image *image)
{
    const struct aetherfs_super *super = &image->super;

    if (aetherfs_le32_to_cpu(super->s_magic) != AETHERFS_MAGIC)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_version) != AETHERFS_VERSION)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_blocksize) != AETHERFS_DEF_BLOCK_SIZE)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_checksum) != aetherfs_super_checksum(super))
        return -EILSEQ;
    if (aetherfs_le64_to_cpu(super->s_blocks_count) < AETHERFS_BOOTSTRAP_BLOCKS)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(super->s_blocks_count) !=
        aetherfs_le64_to_cpu(image->pool_label.pl_logical_blocks))
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_device_count) != image->member_count)
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

    return 0;
}

static int aetherfs_image_validate_checkpoint_root(const struct aetherfs_image *image)
{
    const struct aetherfs_checkpoint_root *root = &image->checkpoint_root;

    if (aetherfs_le32_to_cpu(root->cr_magic) != AETHERFS_CHECKPOINT_ROOT_MAGIC)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_version) != AETHERFS_CHECKPOINT_ROOT_VERSION)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_checksum) !=
        aetherfs_checkpoint_root_checksum(root))
        return -EILSEQ;
    if (aetherfs_le32_to_cpu(root->cr_generation) !=
        aetherfs_le32_to_cpu(image->super.s_generation))
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_ag_count) !=
        aetherfs_le32_to_cpu(image->super.s_ag_count))
        return -EINVAL;
    if (aetherfs_le32_to_cpu(root->cr_region_count) != 2)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_super_block) != AETHERFS_SUPERBLOCK_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_root_inode_block) !=
        aetherfs_le64_to_cpu(image->super.s_root_inode))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_root_dir_block) != AETHERFS_ROOT_DIR_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_journal_block) !=
        aetherfs_le64_to_cpu(image->super.s_journal_addr))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_inode_bitmap_block) !=
        aetherfs_le64_to_cpu(image->super.s_meta_tree))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_free_space_root) !=
        aetherfs_le64_to_cpu(image->super.s_free_tree))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_metadata_tree_root) !=
        aetherfs_le64_to_cpu(image->super.s_meta_tree))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_extent_tree_root) !=
        aetherfs_le64_to_cpu(image->super.s_extent_tree))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_snapshot_root) !=
        aetherfs_le64_to_cpu(image->super.s_snap_tree))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_scrub_journal) !=
        aetherfs_le64_to_cpu(image->super.s_scrub_journal))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_health_journal) !=
        aetherfs_le64_to_cpu(image->super.s_health_journal))
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_metadata_region_start) != AETHERFS_SUPERBLOCK_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_metadata_region_end) != AETHERFS_CHECKPOINT_ROOT_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_data_region_start) != AETHERFS_DATA_START_BLOCK)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_data_region_end) != image->blocks)
        return -EINVAL;
    if (aetherfs_le64_to_cpu(root->cr_special_metadata_root) != 0 ||
        aetherfs_le64_to_cpu(root->cr_cache_root) != 0 ||
        aetherfs_le64_to_cpu(root->cr_parity_root) != 0)
        return -EINVAL;

    return 0;
}

static int aetherfs_image_validate_bitmap(const struct aetherfs_bitmap_block *bitmap,
    uint32_t expected_bits)
{
    if (aetherfs_le32_to_cpu(bitmap->bb_total_bits) != expected_bits)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(bitmap->bb_checksum) != aetherfs_bitmap_checksum(bitmap))
        return -EILSEQ;
    if (aetherfs_le32_to_cpu(bitmap->bb_set_bits) != aetherfs_image_bitmap_count_set(bitmap))
        return -EILSEQ;
    return 0;
}

int aetherfs_image_load(struct aetherfs_image *image)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    int rc;

    if (!image)
        return -EINVAL;

    rc = aetherfs_image_read_block(image, AETHERFS_SUPERBLOCK_BLOCK, block);
    if (rc < 0)
        return rc;
    memcpy(&image->super, block, sizeof(image->super));

    rc = aetherfs_image_validate_super(image);
    if (rc < 0)
        return rc;

    image->blocks = aetherfs_le64_to_cpu(image->super.s_blocks_count);

    rc = aetherfs_image_read_block(image,
        aetherfs_le64_to_cpu(image->super.s_checkpoint[0]), block);
    if (rc < 0)
        return rc;
    memcpy(&image->checkpoint_root, block, sizeof(image->checkpoint_root));
    rc = aetherfs_image_validate_checkpoint_root(image);
    if (rc < 0)
        return rc;

    rc = aetherfs_image_read_block(image,
        aetherfs_le64_to_cpu(image->checkpoint_root.cr_journal_block), block);
    if (rc < 0)
        return rc;
    memcpy(&image->journal, block, sizeof(image->journal));
    if (aetherfs_le32_to_cpu(image->journal.il_checksum) !=
        aetherfs_journal_checksum(&image->journal))
        return -EILSEQ;

    rc = aetherfs_image_read_block(image,
        aetherfs_le64_to_cpu(image->checkpoint_root.cr_inode_bitmap_block),
        &image->inode_bitmap);
    if (rc < 0)
        return rc;
    rc = aetherfs_image_validate_bitmap(&image->inode_bitmap, AETHERFS_MAX_INODES);
    if (rc < 0)
        return rc;

    rc = aetherfs_read_extent_node(image,
        aetherfs_le64_to_cpu(image->checkpoint_root.cr_free_space_root),
        &image->free_extents);
    if (rc < 0)
        return rc;

    rc = aetherfs_image_read_block(image,
        aetherfs_le64_to_cpu(image->checkpoint_root.cr_root_inode_block),
        image->inode_table);
    if (rc < 0)
        return rc;

    rc = aetherfs_image_read_block(image,
        aetherfs_le64_to_cpu(image->checkpoint_root.cr_root_dir_block),
        image->root_dir);
    if (rc < 0)
        return rc;

    return 0;
}

static int aetherfs_image_validate_root_dir(struct aetherfs_image *image,
    bool dir_refs[AETHERFS_MAX_INODES])
{
    struct aetherfs_dir_entry *entry;
    uint8_t *block = image->root_dir;
    uint32_t offset = 0;
    bool saw_dot = false;
    bool saw_dotdot = false;

    while (offset < AETHERFS_DEF_BLOCK_SIZE) {
        uint16_t rec_len;
        uint16_t name_len;
        uint64_t ino;

        entry = (struct aetherfs_dir_entry *)(block + offset);
        rec_len = aetherfs_le16_to_cpu(entry->rec_len);
        name_len = aetherfs_le16_to_cpu(entry->name_len);
        ino = aetherfs_le64_to_cpu(entry->ino);

        if (!rec_len || (rec_len & 3U) || offset + rec_len > AETHERFS_DEF_BLOCK_SIZE)
            return -EINVAL;
        if (name_len > rec_len - AETHERFS_DIR_ENTRY_BASE_SIZE || name_len > AETHERFS_MAX_NAMELEN)
            return -EINVAL;
        if (ino && entry->checksum != aetherfs_dirent_checksum(entry))
            return -EILSEQ;

        if (ino) {
            if (name_len == 1 && memcmp(entry->name, ".", 1) == 0) {
                if (ino != AETHERFS_ROOT_INO)
                    return -EINVAL;
                saw_dot = true;
            } else if (name_len == 2 && memcmp(entry->name, "..", 2) == 0) {
                if (ino != AETHERFS_ROOT_INO)
                    return -EINVAL;
                saw_dotdot = true;
            } else {
                uint64_t inode_index;

                if (ino < AETHERFS_ROOT_INO || ino >= AETHERFS_ROOT_INO + AETHERFS_MAX_INODES)
                    return -EINVAL;
                inode_index = ino - AETHERFS_ROOT_INO;
                if (dir_refs[inode_index])
                    return -EEXIST;
                dir_refs[inode_index] = true;
            }
        }

        offset += rec_len;
    }

    return (saw_dot && saw_dotdot) ? 0 : -EINVAL;
}

static int aetherfs_image_validate_bootstrap_replicas(struct aetherfs_image *image)
{
    uint8_t baseline[AETHERFS_DEF_BLOCK_SIZE];
    uint8_t candidate[AETHERFS_DEF_BLOCK_SIZE];
    int rc;

    for (uint64_t block = AETHERFS_SUPERBLOCK_BLOCK; block < AETHERFS_DATA_START_BLOCK; ++block) {
        uint32_t mask = aetherfs_pool_role_mask_for_bootstrap_block(image, block);
        int first_member;

        if (aetherfs_mask_count(mask) <= 1)
            continue;

        first_member = aetherfs_mask_first(mask);
        if (first_member < 0)
            return -EINVAL;

        rc = aetherfs_image_read_physical_member(image, (uint32_t)first_member, block, baseline);
        if (rc < 0)
            return rc;

        for (uint32_t ordinal = 1; ordinal < aetherfs_mask_count(mask); ++ordinal) {
            int member_index = aetherfs_mask_nth(mask, ordinal);

            if (member_index < 0)
                return -EINVAL;
            rc = aetherfs_image_read_physical_member(image, (uint32_t)member_index, block, candidate);
            if (rc < 0)
                return rc;
            if (memcmp(baseline, candidate, sizeof(baseline)) != 0)
                return -EILSEQ;
        }
    }

    return 0;
}

static int aetherfs_image_validate_mirror_data(struct aetherfs_image *image,
    const struct aetherfs_interval_list *used)
{
    uint32_t mask = aetherfs_pool_data_mask(image);
    uint8_t baseline[AETHERFS_DEF_BLOCK_SIZE];
    uint8_t candidate[AETHERFS_DEF_BLOCK_SIZE];
    int first_member;
    int rc;

    if (aetherfs_mask_count(mask) <= 1)
        return 0;

    first_member = aetherfs_mask_first(mask);
    if (first_member < 0)
        return -EINVAL;

    for (size_t interval_index = 0; interval_index < used->count; ++interval_index) {
        uint64_t start = used->items[interval_index].start;
        uint64_t len = used->items[interval_index].len;

        if (start < AETHERFS_DATA_START_BLOCK)
            continue;

        for (uint64_t offset = 0; offset < len; ++offset) {
            uint64_t block = start + offset;

            rc = aetherfs_image_read_physical_member(image, (uint32_t)first_member, block, baseline);
            if (rc < 0)
                return rc;

            for (uint32_t ordinal = 1; ordinal < aetherfs_mask_count(mask); ++ordinal) {
                int member_index = aetherfs_mask_nth(mask, ordinal);

                if (member_index < 0)
                    return -EINVAL;
                rc = aetherfs_image_read_physical_member(image, (uint32_t)member_index, block, candidate);
                if (rc < 0)
                    return rc;
                if (memcmp(baseline, candidate, sizeof(baseline)) != 0)
                    return -EILSEQ;
            }
        }
    }

    return 0;
}

static int aetherfs_image_validate_parity(struct aetherfs_image *image,
    const struct aetherfs_interval_list *used)
{
    uint32_t mask = aetherfs_pool_data_mask(image);
    uint8_t parity[AETHERFS_DEF_BLOCK_SIZE];
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    int rc;

    if (aetherfs_mask_count(mask) < 3)
        return -EINVAL;

    for (size_t interval_index = 0; interval_index < used->count; ++interval_index) {
        uint64_t start = used->items[interval_index].start;
        uint64_t len = used->items[interval_index].len;

        if (start < AETHERFS_DATA_START_BLOCK)
            continue;

        for (uint64_t offset = 0; offset < len; ++offset) {
            struct aetherfs_pool_mapping mapping;

            rc = aetherfs_pool_map_parity(image, start + offset, &mapping);
            if (rc < 0)
                return rc;

            memset(parity, 0, sizeof(parity));
            for (uint32_t ordinal = 0; ordinal < aetherfs_mask_count(mask); ++ordinal) {
                int member_index = aetherfs_mask_nth(mask, ordinal);

                if (member_index < 0)
                    return -EINVAL;
                rc = aetherfs_image_read_physical_member(image, (uint32_t)member_index,
                    mapping.member_block, block);
                if (rc < 0)
                    return rc;
                for (size_t byte_index = 0; byte_index < sizeof(parity); ++byte_index)
                    parity[byte_index] ^= block[byte_index];
            }

            for (size_t byte_index = 0; byte_index < sizeof(parity); ++byte_index) {
                if (parity[byte_index] != 0)
                    return -EILSEQ;
            }
        }
    }

    return 0;
}

static int aetherfs_image_validate_pool_redundancy(struct aetherfs_image *image,
    const struct aetherfs_interval_list *used)
{
    int rc;

    rc = aetherfs_image_validate_bootstrap_replicas(image);
    if (rc < 0)
        return rc;

    switch (aetherfs_pool_profile(image)) {
    case AETHERFS_POOL_PROFILE_MIRROR:
        return aetherfs_image_validate_mirror_data(image, used);
    case AETHERFS_POOL_PROFILE_PARITY:
        return aetherfs_image_validate_parity(image, used);
    default:
        return 0;
    }
}

int aetherfs_image_validate(struct aetherfs_image *image, struct aetherfs_scrub_report *report)
{
    bool dir_refs[AETHERFS_MAX_INODES] = {0};
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_interval_list used = {0};
    struct aetherfs_interval_list coverage = {0};
    uint64_t free_blocks = 0;
    uint64_t data_blocks = 0;
    uint64_t root_dir_block = 0;
    uint32_t files = 0;
    uint32_t directories = 0;
    int rc = 0;

    if (!image)
        return -EINVAL;

    if (aetherfs_le32_to_cpu(image->journal.il_outstanding) != 0)
        return -EUCLEAN;

    root_dir_block = aetherfs_le64_to_cpu(image->checkpoint_root.cr_root_dir_block);

    if (!aetherfs_image_bitmap_test(&image->inode_bitmap, 0))
        return -EINVAL;

    rc = aetherfs_image_validate_root_dir(image, dir_refs);
    if (rc < 0)
        goto out;

    rc = aetherfs_interval_list_append(&used, root_dir_block, 1);
    if (rc < 0)
        goto out;

    for (uint32_t index = 0; index < AETHERFS_MAX_INODES; ++index) {
        bool allocated = aetherfs_image_bitmap_test(&image->inode_bitmap, index);
        struct aetherfs_inode *inode = &image->inode_table[index];
        uint16_t mode = aetherfs_le16_to_cpu(inode->i_mode);

        if (!allocated) {
            if (mode != 0)
                rc = -EINVAL;
            if (rc < 0)
                goto out;
            continue;
        }

        if (mode == 0) {
            rc = -EINVAL;
            goto out;
        }
        if (aetherfs_le32_to_cpu(inode->i_checksum) != aetherfs_inode_checksum(inode)) {
            rc = -EILSEQ;
            goto out;
        }

        if (S_ISDIR(mode)) {
            directories++;
            if (index != 0 ||
                aetherfs_le64_to_cpu(inode->i_extent_root) != root_dir_block) {
                rc = -EINVAL;
                goto out;
            }
            continue;
        }

        if (!S_ISREG(mode) || !dir_refs[index]) {
            rc = -EINVAL;
            goto out;
        }

        files++;

        {
            struct aetherfs_extent_node extents;
            uint64_t extent_root = aetherfs_le64_to_cpu(inode->i_extent_root);
            uint64_t logical_cursor = 0;
            uint64_t inode_blocks = aetherfs_le64_to_cpu(inode->i_blocks);
            uint64_t inode_size = aetherfs_le64_to_cpu(inode->i_size);
            uint64_t file_blocks = 0;

            if (extent_root < AETHERFS_DATA_START_BLOCK || extent_root >= image->blocks) {
                rc = -EINVAL;
                goto out;
            }

            rc = aetherfs_interval_list_append(&used, extent_root, 1);
            if (rc < 0)
                goto out;

            rc = aetherfs_read_extent_node(image, extent_root, &extents);
            if (rc < 0)
                goto out;
            if (aetherfs_le64_to_cpu(extents.en_next) != 0) {
                rc = -EOPNOTSUPP;
                goto out;
            }

            for (uint32_t extent_index = 0;
                 extent_index < aetherfs_le32_to_cpu(extents.en_count);
                 ++extent_index) {
                struct aetherfs_extent *extent = &extents.en_extents[extent_index];
                uint64_t lstart = aetherfs_le64_to_cpu(extent->e_lstart);
                uint64_t pstart = aetherfs_le64_to_cpu(extent->e_pstart);
                uint64_t csum_block = aetherfs_le64_to_cpu(extent->e_csum_start);
                uint64_t len = aetherfs_le64_to_cpu(extent->e_len);
                uint64_t validated = 0;

                if (!len || lstart != logical_cursor ||
                    pstart < AETHERFS_DATA_START_BLOCK ||
                    pstart + len > image->blocks ||
                    csum_block < AETHERFS_DATA_START_BLOCK ||
                    csum_block >= image->blocks) {
                    rc = -EINVAL;
                    goto out;
                }

                rc = aetherfs_interval_list_append(&used, pstart, len);
                if (rc < 0)
                    goto out;

                while (validated < len) {
                    struct aetherfs_checksum_block sums;
                    uint32_t entries;
                    uint64_t next;

                    rc = aetherfs_interval_list_append(&used, csum_block, 1);
                    if (rc < 0)
                        goto out;

                    rc = aetherfs_read_checksum_block(image, csum_block, &sums);
                    if (rc < 0)
                        goto out;

                    entries = aetherfs_le32_to_cpu(sums.cb_entries);
                    if (!entries || entries > AETHERFS_CHECKSUMS_PER_BLOCK ||
                        entries > len - validated) {
                        rc = -EINVAL;
                        goto out;
                    }

                    for (uint32_t sum_index = 0; sum_index < entries; ++sum_index) {
                        uint32_t checksum = aetherfs_le32_to_cpu(sums.cb_sums[sum_index]);
                        uint64_t data_block = pstart + validated + sum_index;

                        rc = aetherfs_image_read_block(image, data_block, block);
                        if (rc < 0)
                            goto out;
                        if (aetherfs_crc32c_data(block, sizeof(block)) != checksum) {
                            rc = -EILSEQ;
                            goto out;
                        }
                    }

                    validated += entries;
                    next = aetherfs_le64_to_cpu(sums.cb_next);
                    if (validated < len) {
                        if (next < AETHERFS_DATA_START_BLOCK || next >= image->blocks) {
                            rc = -EINVAL;
                            goto out;
                        }
                        csum_block = next;
                    } else if (next != 0) {
                        rc = -EINVAL;
                        goto out;
                    }
                }

                logical_cursor += len;
                file_blocks += len;
                data_blocks += len;
            }

            if (file_blocks != inode_blocks) {
                rc = -EINVAL;
                goto out;
            }
            if ((!file_blocks && inode_size != 0) ||
                (file_blocks &&
                 (inode_size > file_blocks * (uint64_t)AETHERFS_DEF_BLOCK_SIZE ||
                  inode_size <= (file_blocks - 1) * (uint64_t)AETHERFS_DEF_BLOCK_SIZE))) {
                rc = -EINVAL;
                goto out;
            }
        }
    }

    {
        uint32_t free_count = aetherfs_le32_to_cpu(image->free_extents.en_count);
        uint64_t last_end = AETHERFS_DATA_START_BLOCK;

        if (aetherfs_le64_to_cpu(image->free_extents.en_next) != 0 ||
            free_count > AETHERFS_EXTENT_LOG_MAX) {
            rc = -EINVAL;
            goto out;
        }

        for (uint32_t index = 0; index < free_count; ++index) {
            uint64_t start = aetherfs_le64_to_cpu(image->free_extents.en_extents[index].e_pstart);
            uint64_t len = aetherfs_le64_to_cpu(image->free_extents.en_extents[index].e_len);

            if (!len || start < last_end || start + len > image->blocks) {
                rc = -EINVAL;
                goto out;
            }

            rc = aetherfs_interval_list_append(&coverage, start, len);
            if (rc < 0)
                goto out;
            free_blocks += len;
            last_end = start + len;
        }
    }

    qsort(used.items, used.count, sizeof(*used.items), aetherfs_interval_compare);
    for (size_t index = 0; index < used.count; ++index) {
        if (used.items[index].start < AETHERFS_DATA_START_BLOCK)
            continue;
        rc = aetherfs_interval_list_append(&coverage, used.items[index].start, used.items[index].len);
        if (rc < 0)
            goto out;
    }

    qsort(coverage.items, coverage.count, sizeof(*coverage.items), aetherfs_interval_compare);
    {
        uint64_t cursor = AETHERFS_DATA_START_BLOCK;

        for (size_t index = 0; index < coverage.count; ++index) {
            if (coverage.items[index].start != cursor) {
                rc = -EINVAL;
                goto out;
            }
            cursor = coverage.items[index].start + coverage.items[index].len;
        }
        if (cursor != image->blocks) {
            rc = -EINVAL;
            goto out;
        }
    }

    if (aetherfs_le64_to_cpu(image->super.s_free_blocks) != free_blocks) {
        rc = -EINVAL;
        goto out;
    }

    rc = aetherfs_image_validate_pool_redundancy(image, &used);
    if (rc < 0)
        goto out;

    if (report) {
        report->files = files;
        report->directories = directories;
        report->inodes_used = aetherfs_le32_to_cpu(image->inode_bitmap.bb_set_bits);
        report->data_blocks = data_blocks;
        report->free_blocks = free_blocks;
    }

out:
    aetherfs_interval_list_reset(&used);
    aetherfs_interval_list_reset(&coverage);
    return rc;
}

int aetherfs_image_write_super(struct aetherfs_image *image)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE] = {0};

    image->super.s_checksum = aetherfs_cpu_to_le32(aetherfs_super_checksum(&image->super));
    memcpy(block, &image->super, sizeof(image->super));
    return aetherfs_image_write_block(image, AETHERFS_SUPERBLOCK_BLOCK, block);
}

int aetherfs_image_write_journal(struct aetherfs_image *image)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE] = {0};

    image->journal.il_checksum = aetherfs_cpu_to_le32(aetherfs_journal_checksum(&image->journal));
    memcpy(block, &image->journal, sizeof(image->journal));
    return aetherfs_image_write_block(image, AETHERFS_JOURNAL_BLOCK, block);
}

int aetherfs_image_write_inode_table(struct aetherfs_image *image)
{
    return aetherfs_image_write_block(image, AETHERFS_ROOT_INODE_BLOCK, image->inode_table);
}

int aetherfs_image_write_free_extents(struct aetherfs_image *image)
{
    return aetherfs_write_extent_node(image, AETHERFS_FREE_EXTENT_BLOCK, &image->free_extents);
}

static int aetherfs_dir_has_space(const char *path, uint64_t required_bytes)
{
    struct statvfs vfs;
    unsigned long long available;

    if (statvfs(path, &vfs) < 0)
        return 0;

    available = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
    return available >= required_bytes;
}

static int aetherfs_make_temp_path_in_dir(const char *dir, const char *name, char **temp_path_out)
{
    char *path;
    size_t len;
    int fd;

    len = strlen(dir) + 1 + strlen(name) + strlen(".rebalance.XXXXXX") + 1;
    path = malloc(len);
    if (!path)
        return -ENOMEM;

    snprintf(path, len, "%s/%s.rebalance.XXXXXX", dir, name);
    fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return -errno;
    }

    close(fd);
    *temp_path_out = path;
    return 0;
}

int aetherfs_image_make_temp_path(const char *source_path, uint64_t required_bytes,
    char **temp_path_out)
{
    struct stat st;
    char *dirbuf = NULL;
    char *basebuf = NULL;
    const char *candidates[5];
    const char *name;
    size_t candidate_count = 0;
    int rc;

    if (!source_path || !temp_path_out)
        return -EINVAL;

    if (stat(source_path, &st) < 0)
        return -errno;

    basebuf = strdup(source_path);
    if (!basebuf)
        return -ENOMEM;
    name = basename(basebuf);

    if (!S_ISBLK(st.st_mode)) {
        dirbuf = strdup(source_path);
        if (!dirbuf) {
            free(basebuf);
            return -ENOMEM;
        }
        candidates[candidate_count++] = dirname(dirbuf);
    }

    candidates[candidate_count++] = "/var/tmp";
    candidates[candidate_count++] = "/home";
    candidates[candidate_count++] = "/";
    candidates[candidate_count++] = "/tmp";

    for (size_t index = 0; index < candidate_count; ++index) {
        if (!aetherfs_dir_has_space(candidates[index], required_bytes))
            continue;

        rc = aetherfs_make_temp_path_in_dir(candidates[index], name, temp_path_out);
        free(dirbuf);
        free(basebuf);
        return rc;
    }

    free(dirbuf);
    free(basebuf);
    return -ENOSPC;
}
