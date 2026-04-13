#ifndef AETHERFS_HEALTH_H
#define AETHERFS_HEALTH_H

#include "image.h"

struct aetherfs_admin_options {
    const char *path;
    uint32_t member_count;
    struct aetherfs_device_spec members[AETHERFS_MAX_DEVICES];
    const char *output_path;
};

int aetherfs_run_health(const struct aetherfs_admin_options *options);
int aetherfs_run_forensics(const struct aetherfs_admin_options *options);

#endif
