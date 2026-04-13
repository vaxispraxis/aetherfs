#ifndef AETHERFS_IMAGE_H
#define AETHERFS_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "../kernel/include/uapi/linux/aetherfs_format.h"

struct aetherfs_device_spec {
    const char *path;
    uint32_t class_mask;
};

struct aetherfs_image_member {
    int fd;
    bool is_block_device;
    uint64_t size_bytes;
    uint64_t blocks;
    uint32_t class_mask;
    uint32_t role_mask;
    uint32_t member_index;
};

struct aetherfs_image {
    int fd;
    bool writable;
    bool is_block_device;
    uint64_t size_bytes;
    uint64_t blocks;
    uint32_t member_count;
    struct aetherfs_pool_label pool_label;
    struct aetherfs_checkpoint_root checkpoint_root;
    struct aetherfs_image_member members[AETHERFS_MAX_DEVICES];
    struct aetherfs_super super;
    struct aetherfs_intent_log journal;
    struct aetherfs_bitmap_block inode_bitmap;
    struct aetherfs_extent_node free_extents;
    struct aetherfs_inode inode_table[AETHERFS_MAX_INODES];
    uint8_t root_dir[AETHERFS_DEF_BLOCK_SIZE];
};

struct aetherfs_scrub_report {
    uint32_t files;
    uint32_t directories;
    uint32_t inodes_used;
    uint64_t data_blocks;
    uint64_t free_blocks;
};

int aetherfs_image_open(const char *path, bool writable, struct aetherfs_image *image);
int aetherfs_image_open_pool(const struct aetherfs_device_spec *members, uint32_t member_count,
    bool writable, struct aetherfs_image *image);
void aetherfs_image_close(struct aetherfs_image *image);
int aetherfs_image_load(struct aetherfs_image *image);
int aetherfs_image_validate(struct aetherfs_image *image, struct aetherfs_scrub_report *report);
int aetherfs_image_write_block(struct aetherfs_image *image, uint64_t block, const void *buf);
int aetherfs_image_read_block(struct aetherfs_image *image, uint64_t block, void *buf);
int aetherfs_image_write_super(struct aetherfs_image *image);
int aetherfs_image_write_journal(struct aetherfs_image *image);
int aetherfs_image_write_inode_table(struct aetherfs_image *image);
int aetherfs_image_write_free_extents(struct aetherfs_image *image);
uint32_t aetherfs_image_bitmap_count_set(const struct aetherfs_bitmap_block *bitmap);
bool aetherfs_image_bitmap_test(const struct aetherfs_bitmap_block *bitmap, uint32_t bit);
void aetherfs_image_bitmap_set(struct aetherfs_bitmap_block *bitmap, uint32_t bit);
void aetherfs_image_bitmap_clear(struct aetherfs_bitmap_block *bitmap, uint32_t bit);
int aetherfs_image_make_temp_path(const char *source_path, uint64_t required_bytes,
    char **temp_path_out);

#endif
