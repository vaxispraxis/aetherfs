#ifndef AETHERFS_MKFS_H
#define AETHERFS_MKFS_H

#include <stdbool.h>
#include <stdint.h>

#include "image.h"

struct aetherfs_mkfs_options {
    const char *path;
    uint64_t blocks;
    uint32_t block_size;
    uint16_t mode;
    uint32_t pool_profile;
    uint32_t stripe_blocks;
    uint32_t member_count;
    struct aetherfs_device_spec members[AETHERFS_MAX_DEVICES];
    bool force;
};

int aetherfs_run_mkfs(const struct aetherfs_mkfs_options *options);

#endif
