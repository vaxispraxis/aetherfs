#define _POSIX_C_SOURCE 200809L

#include "mount.h"
#include "mkfs.h"
#include "rebalance.h"
#include "health.h"
#include "scrub.h"

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/include/uapi/linux/aetherfs_format.h"

static const char *aetherfs_program_path;

static void aetherfs_usage(FILE *stream)
{
    fprintf(stream,
        "Usage:\n"
        "  aetherfs mkfs [--blocks N] [--block-size 4096] [--mode cow|overwrite|append] [--force] <target>\n"
        "  aetherfs mount [--mkdir] [--module PATH] [--strict|--degraded-ro|--rescue] <image> <mountpoint>\n"
        "  aetherfs scrub [--device CLASSES=PATH ...] <image>\n"
        "  aetherfs health [--device CLASSES=PATH ...] <image>\n"
        "  aetherfs forensics [--device CLASSES=PATH ...] <image> <output-dir>\n"
        "  aetherfs rebalance <image>\n");
}

static int aetherfs_parse_u64(const char *value, uint64_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -EINVAL;

    *out = (uint64_t)parsed;
    return 0;
}

static int aetherfs_parse_u32(const char *value, uint32_t *out)
{
    uint64_t parsed;
    int rc;

    rc = aetherfs_parse_u64(value, &parsed);
    if (rc < 0 || parsed > UINT32_MAX)
        return -EINVAL;

    *out = (uint32_t)parsed;
    return 0;
}

static int aetherfs_parse_mode(const char *value, uint16_t *out)
{
    if (strcmp(value, "cow") == 0) {
        *out = AETHERFS_COW_MODE;
        return 0;
    }
    if (strcmp(value, "overwrite") == 0) {
        *out = AETHERFS_OVERWRITE_MODE;
        return 0;
    }
    if (strcmp(value, "append") == 0) {
        *out = AETHERFS_APPEND_MODE;
        return 0;
    }

    return -EINVAL;
}

static int aetherfs_parse_device_classes(const char *value, uint32_t *mask)
{
    char *copy;
    char *save = NULL;
    char *token;

    *mask = 0;
    if (!value || !*value)
        return 0;

    copy = strdup(value);
    if (!copy)
        return -ENOMEM;

    for (token = strtok_r(copy, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
        if (strcmp(token, "fast") == 0)
            *mask |= AETHERFS_DEV_CLASS_FAST;
        else if (strcmp(token, "capacity") == 0)
            *mask |= AETHERFS_DEV_CLASS_CAPACITY;
        else if (strcmp(token, "archival") == 0)
            *mask |= AETHERFS_DEV_CLASS_ARCHIVAL;
        else if (strcmp(token, "metadata") == 0)
            *mask |= AETHERFS_DEV_CLASS_METADATA;
        else if (strcmp(token, "log") == 0)
            *mask |= AETHERFS_DEV_CLASS_LOG;
        else if (strcmp(token, "cache") == 0)
            *mask |= AETHERFS_DEV_CLASS_CACHE;
        else {
            free(copy);
            return -EINVAL;
        }
    }

    free(copy);
    return 0;
}

static int aetherfs_parse_device_spec(const char *value, struct aetherfs_device_spec *out,
    char **owned)
{
    char *copy;
    char *equals;
    int rc;

    if (!value || !out || !owned)
        return -EINVAL;

    copy = strdup(value);
    if (!copy)
        return -ENOMEM;

    equals = strchr(copy, '=');
    if (equals) {
        *equals = '\0';
        rc = aetherfs_parse_device_classes(copy, &out->class_mask);
        if (rc < 0) {
            free(copy);
            return rc;
        }
        out->path = equals + 1;
    } else {
        out->class_mask = 0;
        out->path = copy;
    }

    if (!out->path || !*out->path) {
        free(copy);
        return -EINVAL;
    }

    *owned = copy;
    return 0;
}

static void aetherfs_free_device_specs(char *owned[], uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
        free(owned[index]);
}

static int aetherfs_cmd_mkfs(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"blocks", required_argument, NULL, 'b'},
        {"block-size", required_argument, NULL, 'B'},
        {"mode", required_argument, NULL, 'm'},
        {"force", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    struct aetherfs_mkfs_options options = {
        .block_size = AETHERFS_DEF_BLOCK_SIZE,
        .mode = AETHERFS_COW_MODE,
    };
    int rc;
    int opt;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "b:B:m:fh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'b':
            rc = aetherfs_parse_u64(optarg, &options.blocks);
            if (rc < 0) {
                fprintf(stderr, "invalid block count: %s\n", optarg);
                return 2;
            }
            break;
        case 'B':
            rc = aetherfs_parse_u32(optarg, &options.block_size);
            if (rc < 0) {
                fprintf(stderr, "invalid block size: %s\n", optarg);
                return 2;
            }
            break;
        case 'm':
            rc = aetherfs_parse_mode(optarg, &options.mode);
            if (rc < 0) {
                fprintf(stderr, "invalid mode: %s\n", optarg);
                return 2;
            }
            break;
        case 'f':
            options.force = true;
            break;
        case 'h':
            aetherfs_usage(stdout);
            return 0;
        default:
            aetherfs_usage(stderr);
            return 2;
        }
    }

    if (optind >= argc) {
        aetherfs_usage(stderr);
        return 2;
    }

    options.path = argv[optind];
    rc = aetherfs_run_mkfs(&options);
    if (rc < 0) {
        fprintf(stderr, "mkfs failed: %s\n", strerror(-rc));
        return 1;
    }

    return 0;
}

static int aetherfs_cmd_mount(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"mkdir", no_argument, NULL, 'p'},
        {"module", required_argument, NULL, 'm'},
        {"strict", no_argument, NULL, 's'},
        {"degraded-ro", no_argument, NULL, 'd'},
        {"rescue", no_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    struct aetherfs_mount_options options = {
        .behavior = AETHERFS_MOUNT_STRICT,
    };
    char *default_module = NULL;
    int opt;
    int rc;

    default_module = aetherfs_default_module_path(aetherfs_program_path);
    if (default_module)
        options.module_path = default_module;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "pm:sdrh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            options.create_mountpoint = true;
            break;
        case 'm':
            options.module_path = optarg;
            break;
        case 's':
            options.behavior = AETHERFS_MOUNT_STRICT;
            break;
        case 'd':
            options.behavior = AETHERFS_MOUNT_DEGRADED_RO;
            break;
        case 'r':
            options.behavior = AETHERFS_MOUNT_RESCUE;
            break;
        case 'h':
            aetherfs_usage(stdout);
            free(default_module);
            return 0;
        default:
            aetherfs_usage(stderr);
            free(default_module);
            return 2;
        }
    }

    if (argc - optind != 2) {
        aetherfs_usage(stderr);
        free(default_module);
        return 2;
    }

    options.image_path = argv[optind];
    options.mount_path = argv[optind + 1];

    rc = aetherfs_run_mount(&options);
    if (rc < 0) {
        fprintf(stderr, "mount failed: %s\n", strerror(-rc));
        if (rc == -EPERM)
            fprintf(stderr, "hint: mounting the image requires privileges and a loaded aetherfs kernel module\n");
        free(default_module);
        return 1;
    }

    free(default_module);
    return 0;
}

static int aetherfs_cmd_scrub(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    struct aetherfs_scrub_options options = {0};
    char *owned[AETHERFS_MAX_DEVICES] = {0};
    int opt;
    int rc;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            if (options.member_count >= AETHERFS_MAX_DEVICES) {
                fprintf(stderr, "too many devices\n");
                aetherfs_free_device_specs(owned, options.member_count);
                return 2;
            }
            rc = aetherfs_parse_device_spec(optarg, &options.members[options.member_count],
                &owned[options.member_count]);
            if (rc < 0) {
                fprintf(stderr, "invalid device specification: %s\n", optarg);
                aetherfs_free_device_specs(owned, options.member_count);
                return 2;
            }
            options.member_count++;
            break;
        case 'h':
            aetherfs_usage(stdout);
            aetherfs_free_device_specs(owned, options.member_count);
            return 0;
        default:
            aetherfs_usage(stderr);
            aetherfs_free_device_specs(owned, options.member_count);
            return 2;
        }
    }

    if (options.member_count == 0) {
        if (argc - optind != 1) {
            aetherfs_usage(stderr);
            return 2;
        }
        options.path = argv[optind];
    } else if (argc - optind != 0) {
        aetherfs_usage(stderr);
        aetherfs_free_device_specs(owned, options.member_count);
        return 2;
    }

    rc = aetherfs_run_scrub(&options);
    aetherfs_free_device_specs(owned, options.member_count);
    if (rc < 0) {
        fprintf(stderr, "scrub failed: %s\n", strerror(-rc));
        return 1;
    }

    return 0;
}

static int aetherfs_cmd_health(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    struct aetherfs_admin_options options = {0};
    char *owned[AETHERFS_MAX_DEVICES] = {0};
    int opt;
    int rc;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            if (options.member_count >= AETHERFS_MAX_DEVICES) {
                fprintf(stderr, "too many devices\n");
                aetherfs_free_device_specs(owned, options.member_count);
                return 2;
            }
            rc = aetherfs_parse_device_spec(optarg, &options.members[options.member_count],
                &owned[options.member_count]);
            if (rc < 0) {
                fprintf(stderr, "invalid device specification: %s\n", optarg);
                aetherfs_free_device_specs(owned, options.member_count);
                return 2;
            }
            options.member_count++;
            break;
        case 'h':
            aetherfs_usage(stdout);
            aetherfs_free_device_specs(owned, options.member_count);
            return 0;
        default:
            aetherfs_usage(stderr);
            aetherfs_free_device_specs(owned, options.member_count);
            return 2;
        }
    }

    if (options.member_count == 0) {
        if (argc - optind != 1) {
            aetherfs_usage(stderr);
            return 2;
        }
        options.path = argv[optind];
    } else if (argc - optind != 0) {
        aetherfs_usage(stderr);
        aetherfs_free_device_specs(owned, options.member_count);
        return 2;
    }

    rc = aetherfs_run_health(&options);
    aetherfs_free_device_specs(owned, options.member_count);
    if (rc < 0) {
        fprintf(stderr, "health failed: %s\n", strerror(-rc));
        return 1;
    }

    return 0;
}

static int aetherfs_cmd_forensics(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    struct aetherfs_admin_options options = {0};
    char *owned[AETHERFS_MAX_DEVICES] = {0};
    int opt;
    int rc;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            if (options.member_count >= AETHERFS_MAX_DEVICES) {
                fprintf(stderr, "too many devices\n");
                aetherfs_free_device_specs(owned, options.member_count);
                return 2;
            }
            rc = aetherfs_parse_device_spec(optarg, &options.members[options.member_count],
                &owned[options.member_count]);
            if (rc < 0) {
                fprintf(stderr, "invalid device specification: %s\n", optarg);
                aetherfs_free_device_specs(owned, options.member_count);
                return 2;
            }
            options.member_count++;
            break;
        case 'h':
            aetherfs_usage(stdout);
            aetherfs_free_device_specs(owned, options.member_count);
            return 0;
        default:
            aetherfs_usage(stderr);
            aetherfs_free_device_specs(owned, options.member_count);
            return 2;
        }
    }

    if (options.member_count == 0) {
        if (argc - optind != 2) {
            aetherfs_usage(stderr);
            return 2;
        }
        options.path = argv[optind];
        options.output_path = argv[optind + 1];
    } else {
        if (argc - optind != 1) {
            aetherfs_usage(stderr);
            aetherfs_free_device_specs(owned, options.member_count);
            return 2;
        }
        options.output_path = argv[optind];
    }

    rc = aetherfs_run_forensics(&options);
    aetherfs_free_device_specs(owned, options.member_count);
    if (rc < 0) {
        fprintf(stderr, "forensics failed: %s\n", strerror(-rc));
        return 1;
    }

    return 0;
}

static int aetherfs_cmd_rebalance(int argc, char **argv)
{
    int rc;

    if (argc != 2) {
        aetherfs_usage(stderr);
        return 2;
    }

    rc = aetherfs_run_rebalance(argv[1]);
    if (rc < 0) {
        fprintf(stderr, "rebalance failed: %s\n", strerror(-rc));
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    aetherfs_program_path = argv[0];

    if (argc < 2) {
        aetherfs_usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "mkfs") == 0)
        return aetherfs_cmd_mkfs(argc - 1, argv + 1);
    if (strcmp(argv[1], "mount") == 0)
        return aetherfs_cmd_mount(argc - 1, argv + 1);
    if (strcmp(argv[1], "scrub") == 0)
        return aetherfs_cmd_scrub(argc - 1, argv + 1);
    if (strcmp(argv[1], "health") == 0)
        return aetherfs_cmd_health(argc - 1, argv + 1);
    if (strcmp(argv[1], "forensics") == 0)
        return aetherfs_cmd_forensics(argc - 1, argv + 1);
    if (strcmp(argv[1], "rebalance") == 0)
        return aetherfs_cmd_rebalance(argc - 1, argv + 1);

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        aetherfs_usage(stdout);
        return 0;
    }

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    aetherfs_usage(stderr);
    return 2;
}
