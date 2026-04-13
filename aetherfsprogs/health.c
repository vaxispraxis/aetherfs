#define _POSIX_C_SOURCE 200809L

#include "health.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int aetherfs_open_admin_image(const struct aetherfs_admin_options *options,
    struct aetherfs_image *image)
{
    if (!options)
        return -EINVAL;

    if (options->member_count > 0)
        return aetherfs_image_open_pool(options->members, options->member_count, false, image);
    if (!options->path)
        return -EINVAL;
    return aetherfs_image_open(options->path, false, image);
}

static const char *aetherfs_profile_name(uint32_t profile)
{
    switch (profile) {
    case AETHERFS_POOL_PROFILE_SINGLE:
        return "single";
    case AETHERFS_POOL_PROFILE_MIRROR:
        return "mirror";
    case AETHERFS_POOL_PROFILE_STRIPE:
        return "stripe";
    case AETHERFS_POOL_PROFILE_PARITY:
        return "parity";
    case AETHERFS_POOL_PROFILE_TIERED:
        return "tiered";
    default:
        return "unknown";
    }
}

static const char *aetherfs_mount_recommendation(int load_rc, int validate_rc)
{
    if (load_rc == 0 && validate_rc == 0)
        return "strict";
    if (load_rc == 0)
        return "degraded-read-only";
    return "rescue";
}

static int aetherfs_make_dir_if_missing(const char *path)
{
    struct stat st;

    if (!path)
        return -EINVAL;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
    if (mkdir(path, 0755) < 0)
        return -errno;
    return 0;
}

static int aetherfs_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *cursor = buf;
    size_t written = 0;

    while (written < len) {
        ssize_t rc = write(fd, cursor + written, len - written);
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

static int aetherfs_copy_block_to_file(const char *source, const char *dest, uint64_t block)
{
    uint8_t buffer[AETHERFS_DEF_BLOCK_SIZE];
    int in_fd;
    int out_fd;
    ssize_t rc;
    int ret;

    in_fd = open(source, O_RDONLY | O_CLOEXEC);
    if (in_fd < 0)
        return -errno;

    rc = pread(in_fd, buffer, sizeof(buffer), (off_t)(block * (uint64_t)AETHERFS_DEF_BLOCK_SIZE));
    close(in_fd);
    if (rc < 0)
        return -errno;
    if ((size_t)rc != sizeof(buffer))
        return -EIO;

    out_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out_fd < 0)
        return -errno;

    ret = aetherfs_write_all(out_fd, buffer, sizeof(buffer));
    close(out_fd);
    return ret;
}

static int aetherfs_write_report(const char *path, const struct aetherfs_image *image,
    int load_rc, int validate_rc)
{
    char report[4096];
    int fd;
    int len;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -errno;

    if (load_rc == 0) {
        len = snprintf(report, sizeof(report),
            "status: %s\n"
            "profile: %s\n"
            "members: %u\n"
            "checkpoint_root: %llu\n"
            "logical_blocks: %llu\n"
            "data_region: %llu-%llu\n"
            "free_blocks: %llu\n"
            "load_result: ok\n"
            "scrub_result: %s\n"
            "recommended_mount: %s\n",
            validate_rc == 0 ? "clean" : "damaged",
            aetherfs_profile_name(aetherfs_le32_to_cpu(image->pool_label.pl_profile)),
            image->member_count,
            (unsigned long long)aetherfs_le64_to_cpu(image->super.s_checkpoint[0]),
            (unsigned long long)aetherfs_le64_to_cpu(image->pool_label.pl_logical_blocks),
            (unsigned long long)aetherfs_le64_to_cpu(image->checkpoint_root.cr_data_region_start),
            (unsigned long long)aetherfs_le64_to_cpu(image->checkpoint_root.cr_data_region_end),
            (unsigned long long)aetherfs_le64_to_cpu(image->super.s_free_blocks),
            validate_rc == 0 ? "ok" : strerror(-validate_rc),
            aetherfs_mount_recommendation(load_rc, validate_rc));
    } else {
        len = snprintf(report, sizeof(report),
            "status: unreadable\n"
            "load_result: %s\n"
            "recommended_mount: %s\n",
            strerror(-load_rc),
            aetherfs_mount_recommendation(load_rc, validate_rc));
    }

    if (len < 0 || (size_t)len >= sizeof(report)) {
        close(fd);
        return -EOVERFLOW;
    }

    validate_rc = aetherfs_write_all(fd, report, (size_t)len);
    close(fd);
    return validate_rc;
}

int aetherfs_run_health(const struct aetherfs_admin_options *options)
{
    struct aetherfs_image *image = NULL;
    struct aetherfs_scrub_report report = {0};
    int load_rc;
    int validate_rc;
    int rc;

    image = calloc(1, sizeof(*image));
    if (!image)
        return -ENOMEM;

    rc = aetherfs_open_admin_image(options, image);
    if (rc < 0)
        goto out;

    load_rc = aetherfs_image_load(image);
    if (load_rc == 0)
        validate_rc = aetherfs_image_validate(image, &report);
    else
        validate_rc = load_rc;

    if (load_rc == 0) {
        printf("health: %s\n", validate_rc == 0 ? "clean" : "damaged");
        printf("profile: %s\n",
            aetherfs_profile_name(aetherfs_le32_to_cpu(image->pool_label.pl_profile)));
        printf("members: %u\n", image->member_count);
        printf("checkpoint root: %llu\n",
            (unsigned long long)aetherfs_le64_to_cpu(image->super.s_checkpoint[0]));
        printf("logical blocks: %llu\n",
            (unsigned long long)aetherfs_le64_to_cpu(image->pool_label.pl_logical_blocks));
        printf("data region: %llu-%llu\n",
            (unsigned long long)aetherfs_le64_to_cpu(image->checkpoint_root.cr_data_region_start),
            (unsigned long long)aetherfs_le64_to_cpu(image->checkpoint_root.cr_data_region_end));
        printf("free blocks: %llu\n",
            (unsigned long long)aetherfs_le64_to_cpu(image->super.s_free_blocks));
        if (validate_rc == 0) {
            printf("scrub: clean (files=%u directories=%u data_blocks=%llu)\n",
                report.files,
                report.directories,
                (unsigned long long)report.data_blocks);
        } else {
            printf("scrub: %s\n", strerror(-validate_rc));
        }
        printf("recommended mount: %s\n",
            aetherfs_mount_recommendation(load_rc, validate_rc));
    } else {
        printf("health: unreadable\n");
        printf("load: %s\n", strerror(-load_rc));
        printf("recommended mount: %s\n",
            aetherfs_mount_recommendation(load_rc, validate_rc));
    }

out:
    if (image) {
        aetherfs_image_close(image);
        free(image);
    }
    return rc < 0 ? rc : (validate_rc < 0 ? validate_rc : 0);
}

int aetherfs_run_forensics(const struct aetherfs_admin_options *options)
{
    struct aetherfs_image *image = NULL;
    char path[512];
    int load_rc;
    int validate_rc;
    int rc;

    if (!options || !options->output_path)
        return -EINVAL;

    rc = aetherfs_make_dir_if_missing(options->output_path);
    if (rc < 0)
        return rc;

    image = calloc(1, sizeof(*image));
    if (!image)
        return -ENOMEM;

    rc = aetherfs_open_admin_image(options, image);
    if (rc < 0)
        goto out;

    load_rc = aetherfs_image_load(image);
    if (load_rc == 0)
        validate_rc = aetherfs_image_validate(image, NULL);
    else
        validate_rc = load_rc;

    for (uint32_t member = 0; member < (options->member_count ? options->member_count : 1U); ++member) {
        const char *source = options->member_count ? options->members[member].path : options->path;

        for (uint64_t block = AETHERFS_POOL_LABEL_BLOCK; block < AETHERFS_DATA_START_BLOCK; ++block) {
            snprintf(path, sizeof(path), "%s/member%u-block%llu.bin",
                options->output_path,
                member,
                (unsigned long long)block);
            rc = aetherfs_copy_block_to_file(source, path, block);
            if (rc < 0) {
                goto out;
            }
        }
    }

    snprintf(path, sizeof(path), "%s/report.txt", options->output_path);
    rc = aetherfs_write_report(path, image, load_rc, validate_rc);
    if (rc < 0)
        goto out;

    printf("forensics exported to %s\n", options->output_path);

out:
    if (image) {
        aetherfs_image_close(image);
        free(image);
    }
    return rc < 0 ? rc : (validate_rc < 0 ? validate_rc : 0);
}
