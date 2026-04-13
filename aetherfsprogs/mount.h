#ifndef AETHERFS_MOUNT_H
#define AETHERFS_MOUNT_H

#include <stdbool.h>

enum aetherfs_mount_behavior {
    AETHERFS_MOUNT_STRICT = 0,
    AETHERFS_MOUNT_DEGRADED_RO = 1,
    AETHERFS_MOUNT_RESCUE = 2,
};

struct aetherfs_mount_options {
    const char *image_path;
    const char *mount_path;
    const char *module_path;
    bool create_mountpoint;
    enum aetherfs_mount_behavior behavior;
};

int aetherfs_run_mount(const struct aetherfs_mount_options *options);
char *aetherfs_default_module_path(const char *argv0);

#endif
