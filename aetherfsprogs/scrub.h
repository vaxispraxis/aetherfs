#ifndef AETHERFS_SCRUB_H
#define AETHERFS_SCRUB_H

#include "image.h"

struct aetherfs_scrub_options {
    const char *path;
    uint32_t member_count;
    struct aetherfs_device_spec members[AETHERFS_MAX_DEVICES];
};

int aetherfs_run_scrub(const struct aetherfs_scrub_options *options);

#endif
