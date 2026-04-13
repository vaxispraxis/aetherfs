#include "scrub.h"

#include "image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static const char *aetherfs_scrub_profile_name(uint32_t profile)
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

int aetherfs_run_scrub(const struct aetherfs_scrub_options *options)
{
    struct aetherfs_image *image = NULL;
    struct aetherfs_scrub_report report = {0};
    int rc;

    if (!options)
        return -EINVAL;

    image = calloc(1, sizeof(*image));
    if (!image)
        return -ENOMEM;

    if (options->member_count > 0)
        rc = aetherfs_image_open_pool(options->members, options->member_count, false, image);
    else
        rc = aetherfs_image_open(options->path, false, image);
    if (rc < 0)
        goto out;

    rc = aetherfs_image_load(image);
    if (rc < 0)
        goto out;

    rc = aetherfs_image_validate(image, &report);
    if (rc < 0)
        goto out;

    printf("scrub: clean\n");
    printf("profile: %s\n",
        aetherfs_scrub_profile_name(aetherfs_le32_to_cpu(image->pool_label.pl_profile)));
    printf("members: %u\n", image->member_count);
    printf("checkpoint root: %llu\n",
        (unsigned long long)aetherfs_le64_to_cpu(image->super.s_checkpoint[0]));
    printf("logical blocks: %llu\n",
        (unsigned long long)aetherfs_le64_to_cpu(image->pool_label.pl_logical_blocks));
    printf("files: %u\n", report.files);
    printf("directories: %u\n", report.directories);
    printf("inodes: %u\n", report.inodes_used);
    printf("data blocks: %llu\n", (unsigned long long)report.data_blocks);
    printf("free blocks: %llu\n", (unsigned long long)report.free_blocks);

out:
    if (image) {
        aetherfs_image_close(image);
        free(image);
    }
    return rc;
}
