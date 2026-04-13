#include "aetherfs.h"

enum {
	AETHERFS_POLICY_DESKTOP = 0,
	AETHERFS_POLICY_DATABASE = 1,
	AETHERFS_POLICY_VMHOST = 2,
	AETHERFS_POLICY_ARCHIVE = 3,
	AETHERFS_POLICY_CONTAINER = 4,
};

static struct aetherfs_policy {
	char p_path[256];
	uint32_t p_mode;
	uint32_t p_compress;
	uint32_t p_checksum;
	uint32_t p_snapshot;
} aetherfs_policies[16];

static int aetherfs_policy_count = 0;

int aetherfs_set_policy(const char *path, uint32_t mode, uint32_t compress,
		    uint32_t checksum, uint32_t snapshot)
{
	struct aetherfs_policy *p;

	if (!path || aetherfs_policy_count >= 16)
		return -EINVAL;

	p = &aetherfs_policies[aetherfs_policy_count++];
	strncpy(p->p_path, path, 255);
	p->p_mode = mode;
	p->p_compress = compress;
	p->p_checksum = checksum;
	p->p_snapshot = snapshot;

	return 0;
}

int aetherfs_get_policy(const char *path, struct aetherfs_policy *out)
{
	struct aetherfs_policy *p;
	int i;
	int best_match = -1;
	int match_len = 0;

	if (!path || !out)
		return -EINVAL;

	for (i = 0; i < aetherfs_policy_count; i++) {
		p = &aetherfs_policies[i];
		if (!strncmp(path, p->p_path, strlen(p->p_path))) {
			if (strlen(p->p_path) > match_len) {
				best_match = i;
				match_len = strlen(p->p_path);
			}
		}
	}

	if (best_match < 0)
		return -ENOENT;

	*out = aetherfs_policies[best_match];
	return 0;
}

int aetherfs_apply_policy(struct inode *inode, const char *path)
{
	struct aetherfs_policy policy;
	struct aetherfs_inode_info *ei;
	int err;

	if (!inode || !path)
		return -EINVAL;

	err = aetherfs_get_policy(path, &policy);
	if (err)
		return 0;

	ei = AETHERFS_INODE(inode);

	switch (policy.p_mode) {
	case AETHERFS_MODE_COW:
		ei->i_flags |= AETHERFS_F_COW;
		break;
	case AETHERFS_MODE_OVERWRITE:
		ei->i_flags &= ~AETHERFS_F_COW;
		break;
	case AETHERFS_MODE_APPEND:
		ei->i_flags |= AETHERFS_F_APPEND;
		break;
	}

	if (policy.p_compress)
		ei->i_flags |= AETHERFS_F_COMPRESSED;

	mark_inode_dirty(inode);
	return 0;
}

int aetherfs_init_default_policies(void)
{
	aetherfs_set_policy("/var/lib/vm", AETHERFS_MODE_OVERWRITE, 0, 1, 0);
	aetherfs_set_policy("/var/lib/docker", AETHERFS_MODE_COW, 0, 1, 1);
	aetherfs_set_policy("/home", AETHERFS_MODE_COW, 1, 1, 1);
	aetherfs_set_policy("/srv/archive", AETHERFS_MODE_COW, 2, 2, 1);
	aetherfs_set_policy("/var/log", AETHERFS_MODE_APPEND, 0, 0, 0);

	return 0;
}

int aetherfs_list_policies(void *arg, void (*callback)(void *, struct aetherfs_policy *))
{
	int i;

	if (!callback)
		return -EINVAL;

	for (i = 0; i < aetherfs_policy_count; i++) {
		callback(arg, &aetherfs_policies[i]);
	}

	return 0;
}