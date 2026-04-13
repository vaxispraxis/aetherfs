#include "aetherfs.h"

struct aetherfs_extent_ref {
	__le64 er_extent_id;
	__le32 er_refcount;
	__le32 er_flags;
#define AETHERFS_EXT_SHARED    0x0001
#define AETHERFS_EXT_PINNED   0x0002
#define AETHERFS_EXT_EXCLUSIVE 0x0004
#define AETHERFS_EXT_COW_PENDING 0x0008
};

struct aetherfs_write_intent {
	__le64 wi_txid;
	__le64 wi_inode;
	__le64 wi_extent_id;
	__le32 wi_mode;
	__le32 wi_flags;
#define AETHERFS_WI_COMMITTED  0x0001
#define AETHERFS_WI_PENDING 0x0002
#define AETHERFS_WI_DISCARDED 0x0004
	__le32 wi_old_checksum;
	__le32 wi_new_checksum;
	__le64 wi_generation;
	__le64 wi_next;
};

struct aetherfs_transaction {
	__le64 tx_id;
	__le64 tx_inode;
	__le64 tx_root_before;
	__le64 tx_root_after;
	__le32 tx_mode;
	__le32 tx_flags;
	__le32 tx_checksum;
	__le32 tx_extent_count;
	__le64 tx_extents[16];
};

int aetherfs_extent_is_exclusive(struct aetherfs_extent_ref *ref)
{
	if (!ref)
		return 0;
	return (le32_to_cpu(ref->er_refcount) == 1) &&
	       !(le32_to_cpu(ref->er_flags) & AETHERFS_EXT_PINNED);
}

int aetherfs_extent_is_shared(struct aetherfs_extent_ref *ref)
{
	if (!ref)
		return 0;
	return le32_to_cpu(ref->er_refcount) > 1 ||
	       (le32_to_cpu(ref->er_flags) & AETHERFS_EXT_SHARED);
}

int aetherfs_extent_acquireExclusive(struct aetherfs_extent_ref *ref)
{
	if (!ref)
		return -EINVAL;

	if (le32_to_cpu(ref->er_refcount) > 1)
		return -EBUSY;

	if (le32_to_cpu(ref->er_flags) & AETHERFS_EXT_PINNED)
		return -EBUSY;

	ref->er_flags |= cpu_to_le32(AETHERFS_EXT_EXCLUSIVE);
	return 0;
}

void aetherfs_extent_releaseExclusive(struct aetherfs_extent_ref *ref)
{
	if (!ref)
		return;
	ref->er_flags &= cpu_to_le32(~AETHERFS_EXT_EXCLUSIVE);
}

static int aetherfs_overwrite_allowed(struct inode *inode, struct aetherfs_extent_ref *ref)
{
	uint32_t mode = aetherfs_get_data_mode(inode);

	if (mode != AETHERFS_MODE_OVERWRITE)
		return 0;

	return aetherfs_extent_is_exclusive(ref) ? 0 : -EBUSY;
}

int aetherfs_write_core(struct inode *inode, sector_t iblock,
		     char *buf, unsigned int len, int mode)
{
	struct aetherfs_extent_ref ref;
	int err;

	memset(&ref, 0, sizeof(ref));
	ref.er_refcount = cpu_to_le32(1);

	switch (mode) {
	case AETHERFS_MODE_OVERWRITE:
		err = aetherfs_overwrite_allowed(inode, &ref);
		if (err == -EBUSY) {
			ref.er_flags |= cpu_to_le32(AETHERFS_EXT_COW_PENDING);
			return aetherfs_write_core(inode, iblock, buf, len, AETHERFS_MODE_COW);
		}
		return aetherfs_mode_overwrite_write(inode, iblock, buf, len);

	case AETHERFS_MODE_COW:
		return aetherfs_mode_cow_write(inode, iblock, buf, len);

	case AETHERFS_MODE_APPEND:
		return aetherfs_mode_append_write(inode, iblock, buf, len);

	default:
		return -EINVAL;
	}
}