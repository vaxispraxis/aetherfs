#define _POSIX_C_SOURCE 200809L

#include "mount.h"
#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kernel/include/uapi/linux/aetherfs_format.h"

#define AETHERFS_KERNEL_MODULE "aetherfs"
#define AETHERFS_MOUNT_TYPE "aetherfs"

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

static int aetherfs_validate_image(const char *path)
{
    uint8_t block[AETHERFS_DEF_BLOCK_SIZE];
    struct aetherfs_pool_label *label = (struct aetherfs_pool_label *)block;
    struct aetherfs_checkpoint_root *checkpoint_root =
        (struct aetherfs_checkpoint_root *)block;
    struct aetherfs_super *super = (struct aetherfs_super *)block;
    struct stat st;
    int fd;
    int rc;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    if (fstat(fd, &st) < 0) {
        rc = -errno;
        close(fd);
        return rc;
    }
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
        close(fd);
        return -EINVAL;
    }

    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_POOL_LABEL_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    if (rc < 0) {
        close(fd);
        return rc;
    }
    if (aetherfs_le32_to_cpu(label->pl_magic) == AETHERFS_POOL_MAGIC) {
        rc = aetherfs_pool_label_checksum(label) == aetherfs_le32_to_cpu(label->pl_checksum)
            ? 0
            : -EILSEQ;
        if (rc < 0) {
            close(fd);
            return rc;
        }
        if (aetherfs_le32_to_cpu(label->pl_member_count) != 1) {
            close(fd);
            return -EOPNOTSUPP;
        }
    }

    rc = aetherfs_read_all(fd, (uint64_t)AETHERFS_SUPERBLOCK_BLOCK * AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    close(fd);
    if (rc < 0)
        return rc;

    if (aetherfs_le32_to_cpu(super->s_magic) != AETHERFS_MAGIC)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_version) != AETHERFS_VERSION)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_blocksize) != AETHERFS_DEF_BLOCK_SIZE)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_device_count) != 1)
        return -EOPNOTSUPP;
    if (aetherfs_le64_to_cpu(super->s_checkpoint[0]) != AETHERFS_CHECKPOINT_ROOT_BLOCK)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(super->s_checksum) != aetherfs_super_checksum(super))
        return -EILSEQ;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    rc = aetherfs_read_all(fd,
        aetherfs_le64_to_cpu(super->s_checkpoint[0]) * (uint64_t)AETHERFS_DEF_BLOCK_SIZE,
        block, sizeof(block));
    close(fd);
    if (rc < 0)
        return rc;
    if (aetherfs_le32_to_cpu(checkpoint_root->cr_magic) != AETHERFS_CHECKPOINT_ROOT_MAGIC)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(checkpoint_root->cr_version) !=
        AETHERFS_CHECKPOINT_ROOT_VERSION)
        return -EINVAL;
    if (aetherfs_le32_to_cpu(checkpoint_root->cr_checksum) !=
        aetherfs_checkpoint_root_checksum(checkpoint_root))
        return -EILSEQ;

    return 0;
}

static int aetherfs_validate_strict_image(const char *path)
{
    struct aetherfs_image *image;
    int rc;

    image = calloc(1, sizeof(*image));
    if (!image)
        return -ENOMEM;

    rc = aetherfs_image_open(path, false, image);
    if (rc < 0)
        goto out;

    rc = aetherfs_image_load(image);
    if (rc == 0)
        rc = aetherfs_image_validate(image, NULL);

out:
    aetherfs_image_close(image);
    free(image);
    return rc;
}

static int aetherfs_source_is_block_device(const char *path, bool *is_block)
{
    struct stat st;

    if (!path || !is_block)
        return -EINVAL;

    if (stat(path, &st) < 0)
        return -errno;

    *is_block = S_ISBLK(st.st_mode);
    return 0;
}

static int aetherfs_ensure_mountpoint(const struct aetherfs_mount_options *options)
{
    struct stat st;

    if (stat(options->mount_path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;

    if (!options->create_mountpoint)
        return -ENOENT;

    if (mkdir(options->mount_path, 0755) < 0)
        return -errno;

    return 0;
}

static int aetherfs_exec_wait(char *const argv[], bool require_root)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0)
        return -errno;

    if (pid == 0) {
        if (require_root && geteuid() != 0) {
            size_t argc = 0;
            char **priv_argv;

            while (argv[argc])
                argc++;

            priv_argv = calloc(argc + 2, sizeof(*priv_argv));
            if (!priv_argv)
                _exit(125);

            priv_argv[0] = "doas";
            for (size_t index = 0; index < argc; ++index)
                priv_argv[index + 1] = argv[index];

            execvp(priv_argv[0], priv_argv);
            _exit(errno == ENOENT ? 127 : 126);
        }

        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -errno;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (WIFEXITED(status))
        return -EPERM;
    if (WIFSIGNALED(status))
        return -EINTR;

    return -EIO;
}

static int aetherfs_filesystem_registered(void)
{
    FILE *fp;
    char line[128];

    fp = fopen("/proc/filesystems", "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, AETHERFS_MOUNT_TYPE)) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int aetherfs_try_load_module(const char *module_path)
{
    char *const modprobe_args[] = {"modprobe", "-q", AETHERFS_KERNEL_MODULE, NULL};
    char *const insmod_args[] = {"insmod", (char *)module_path, NULL};
    int rc;

    rc = aetherfs_exec_wait(modprobe_args, true);
    if (rc == 0 || !module_path)
        return rc;

    if (access(module_path, R_OK) != 0)
        return rc;

    return aetherfs_exec_wait(insmod_args, true);
}

char *aetherfs_default_module_path(const char *argv0)
{
    char resolved[PATH_MAX];
    char *dirbuf = NULL;
    char *dir;
    size_t len;
    char *path;

    if (!argv0 || !realpath(argv0, resolved))
        return NULL;

    dirbuf = strdup(resolved);
    if (!dirbuf)
        return NULL;

    dir = dirname(dirbuf);
    len = strlen(dir) + strlen("/../kernel/aetherfs.ko") + 1;
    path = malloc(len);
    if (!path) {
        free(dirbuf);
        return NULL;
    }

    snprintf(path, len, "%s/../kernel/aetherfs.ko", dir);
    free(dirbuf);
    return path;
}

int aetherfs_run_mount(const struct aetherfs_mount_options *options)
{
    char *const loop_mount_args[] = {
        "mount",
        "-t", AETHERFS_MOUNT_TYPE,
        "-o", "loop",
        (char *)options->image_path,
        (char *)options->mount_path,
        NULL,
    };
    char *const loop_mount_ro_args[] = {
        "mount",
        "-t", AETHERFS_MOUNT_TYPE,
        "-o", "loop,ro",
        (char *)options->image_path,
        (char *)options->mount_path,
        NULL,
    };
    char *const block_mount_args[] = {
        "mount",
        "-t", AETHERFS_MOUNT_TYPE,
        (char *)options->image_path,
        (char *)options->mount_path,
        NULL,
    };
    char *const block_mount_ro_args[] = {
        "mount",
        "-t", AETHERFS_MOUNT_TYPE,
        "-o", "ro",
        (char *)options->image_path,
        (char *)options->mount_path,
        NULL,
    };
    char *const *mount_args = loop_mount_args;
    bool is_block = false;
    bool force_read_only = false;
    int rc;

    if (!options || !options->image_path || !options->mount_path)
        return -EINVAL;

    rc = aetherfs_source_is_block_device(options->image_path, &is_block);
    if (rc < 0)
        return rc;
    switch (options->behavior) {
    case AETHERFS_MOUNT_STRICT:
        rc = aetherfs_validate_strict_image(options->image_path);
        if (rc < 0)
            return rc;
        break;
    case AETHERFS_MOUNT_DEGRADED_RO:
        rc = aetherfs_validate_strict_image(options->image_path);
        if (rc < 0) {
            rc = aetherfs_validate_image(options->image_path);
            if (rc < 0)
                return rc;
            force_read_only = true;
        }
        break;
    case AETHERFS_MOUNT_RESCUE:
        rc = aetherfs_validate_image(options->image_path);
        if (rc < 0)
            return rc;
        force_read_only = true;
        break;
    default:
        return -EINVAL;
    }

    rc = aetherfs_ensure_mountpoint(options);
    if (rc < 0)
        return rc;

    if (!aetherfs_filesystem_registered()) {
        rc = aetherfs_try_load_module(options->module_path);
        if (rc < 0)
            return rc;
    }

    if (is_block)
        mount_args = force_read_only ? block_mount_ro_args : block_mount_args;
    else
        mount_args = force_read_only ? loop_mount_ro_args : loop_mount_args;

    rc = aetherfs_exec_wait(mount_args, true);
    if (rc < 0)
        return rc;

    printf("mounted %s at %s\n", options->image_path, options->mount_path);
    return 0;
}
