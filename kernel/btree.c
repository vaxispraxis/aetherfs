#include "aetherfs.h"
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/cache.h>

#define AETHERFS_BTREE_MIN_ORDER  8
#define AETHERFS_BTREE_MAX_ORDER 128
#define AETHERFS_BTREE_DEF_ORDER  32
#define AETHERFS_EXTENT_PER_LEAF  16
#define AETHERFS_METADATA_PER_LEAF 32
#define AETHERFS_BTREE_DEF_NODE_SIZE 4096

enum aetherfs_btree_node_type {
	BTREE_NODE_INTERNAL = 0,
	BTREE_NODE_LEAF_EXTENT = 1,
	BTREE_NODE_LEAF_METADATA = 2,
	BTREE_NODE_LEAF_DIRECTORY = 3,
};

struct aetherfs_btree_info {
	struct aetherfs_btree_node *bt_root;
	rwlock_t bt_lock;
	atomic_t bt_refs;
	uint32_t bt_order;
	uint32_t bt_node_size;
	uint32_t bt_level;
	struct callback_head bt_rcu;
};

struct aetherfs_btree_extent_leaf {
	__le64 el_start;
	__le64 el_blocks;
	__le32 el_flags;
	__le32 el_checksum;
};

struct aetherfs_btree_metadata_leaf {
	__le64 ml_ino;
	__le64 ml_block;
	__le32 ml_size;
	__le32 ml_flags;
	__le32 ml_checksum;
};

struct aetherfs_btree_internal_entry {
	__le64 ie_child;
	__le64 ie_max_key;
};

static DEFINE_RWLOCK(btree_global_lock);
static struct kmem_cache *btree_node_cache;

static inline void btree_write_lock(void)
{
	write_lock(&btree_global_lock);
}

static inline void btree_write_unlock(void)
{
	write_unlock(&btree_global_lock);
}

static inline void btree_read_lock(void)
{
	read_lock(&btree_global_lock);
}

static inline void btree_read_unlock(void)
{
	read_unlock(&btree_global_lock);
}

static inline uint32_t btree_node_size_for_block(uint32_t block_size)
{
	uint32_t size = block_size;
	if (size > PAGE_SIZE)
		size = PAGE_SIZE;
	size = max_t(uint32_t, size, 4096);
	size = min_t(uint32_t, size, 65536);
	return size;
}

static inline int btree_is_leaf(struct aetherfs_btree_node *node)
{
	return le32_to_cpu(node->bn_level) == 0;
}

static inline int btree_is_full(struct aetherfs_btree_node *node, uint32_t order)
{
	return le32_to_cpu(node->bn_count) >= order;
}

static inline int btree_is_min(struct aetherfs_btree_node *node, uint32_t order)
{
	return le32_to_cpu(node->bn_count) <= order / 2;
}

static inline struct aetherfs_btree_key *btree_keys(struct aetherfs_btree_node *node)
{
	return (struct aetherfs_btree_key *)node->bn_data;
}

static inline int btree_compare_ino(struct aetherfs_btree_key *a, uint64_t ino)
{
	uint64_t a_ino = le64_to_cpu(a->k_ino);
	if (a_ino < ino) return -1;
	if (a_ino > ino) return 1;
	return 0;
}

static inline int btree_compare_extent(struct aetherfs_btree_key *a, uint64_t lblock)
{
	uint64_t a_offset = le64_to_cpu(a->k_offset);
	if (a_offset < lblock) return -1;
	if (a_offset > lblock) return 1;
	return 0;
}

static inline int btree_find_slot_ino(struct aetherfs_btree_node *node, uint64_t ino)
{
	struct aetherfs_btree_key *keys = btree_keys(node);
	int low = 0, high = le32_to_cpu(node->bn_count);
	int mid, cmp;

	while (low < high) {
		mid = (low + high) >> 1;
		cmp = btree_compare_ino(&keys[mid], ino);
		if (cmp < 0)
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

static inline int btree_find_slot_extent(struct aetherfs_btree_node *node, uint64_t lblock)
{
	struct aetherfs_btree_key *keys = btree_keys(node);
	int low = 0, high = le32_to_cpu(node->bn_count);
	int mid, cmp;

	while (low < high) {
		mid = (low + high) >> 1;
		cmp = btree_compare_extent(&keys[mid], lblock);
		if (cmp < 0)
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

static struct aetherfs_btree_node *btree_alloc_node(gfp_t gfp)
{
	struct aetherfs_btree_node *node;
	node = kmem_cache_zalloc(btree_node_cache, gfp);
	if (node) {
		node->bn_level = cpu_to_le32(0);
		node->bn_count = cpu_to_le32(0);
		node->bn_checksum = cpu_to_le32(0);
	}
	return node;
}

static void btree_free_node(struct aetherfs_btree_node *node)
{
	if (node)
		kmem_cache_free(btree_node_cache, node);
}

static uint32_t btree_checksum_node(struct aetherfs_btree_node *node, uint32_t size)
{
	return aetherfs_crc32(node, size - sizeof(uint32_t));
}

int aetherfs_btree_create(struct aetherfs_btree_node **root, uint32_t node_size)
{
	struct aetherfs_btree_node *node;

	if (!root || !node_size)
		return -EINVAL;

	node = btree_alloc_node(GFP_NOFS);
	if (!node)
		return -ENOMEM;

	node->bn_level = cpu_to_le32(0);
	node->bn_count = cpu_to_le32(0);
	*root = node;

	return 0;
}

void aetherfs_btree_destroy(struct aetherfs_btree_node *root)
{
	if (!root)
		return;
	btree_free_node(root);
}

int aetherfs_btree_insert_extent(struct aetherfs_btree_node **root,
				  uint64_t ino, uint64_t lblock,
				  uint64_t pblock, uint64_t blocks)
{
	struct aetherfs_btree_node *node = *root;
	struct aetherfs_btree_key *keys;
	int slot;

	if (!root || !node)
		return -EINVAL;

	btree_write_lock();

	if (btree_is_leaf(node)) {
		if (!btree_is_full(node, AETHERFS_BTREE_DEF_ORDER)) {
			slot = btree_find_slot_extent(node, lblock);
			keys = btree_keys(node);

			memmove(&keys[slot + 1], &keys[slot],
			       (le32_to_cpu(node->bn_count) - slot) * sizeof(*keys));

			keys[slot].k_ino = cpu_to_le64(ino);
			keys[slot].k_offset = cpu_to_le64(lblock);
			keys[slot].k_value = cpu_to_le64(pblock);
			keys[slot].k_len = cpu_to_le32(blocks);
			keys[slot].k_checksum = cpu_to_le32(
				aetherfs_crc32(&keys[slot], sizeof(keys[slot]) - 4));

			node->bn_count = cpu_to_le32(le32_to_cpu(node->bn_count) + 1);
			node->bn_checksum = cpu_to_le32(
				btree_checksum_node(node, AETHERFS_BTREE_DEF_NODE_SIZE));
		}
	}

	btree_write_unlock();
	return 0;
}

int aetherfs_btree_lookup_extent(struct aetherfs_btree_node *root,
				 uint64_t ino, uint64_t lblock,
				 uint64_t *pblock, uint32_t *blocks)
{
	struct aetherfs_btree_key key, *keys;
	struct aetherfs_btree_node *node;
	int slot;

	if (!root || !pblock || !blocks)
		return -EINVAL;

	key.k_ino = cpu_to_le64(ino);
	key.k_offset = cpu_to_le64(lblock);

	node = root;
	btree_read_lock();

	while (node) {
		slot = btree_find_slot_extent(node, lblock);
		keys = btree_keys(node);

		if (slot < le32_to_cpu(node->bn_count) &&
		    keys[slot].k_ino == key.k_ino &&
		    keys[slot].k_offset == key.k_offset) {
			*pblock = le64_to_cpu(keys[slot].k_value);
			*blocks = le32_to_cpu(keys[slot].k_len);
			btree_read_unlock();
			return 0;
		}

		if (btree_is_leaf(node))
			break;
	}

	btree_read_unlock();
	return -ENOENT;
}

int aetherfs_btree_insert_metadata(struct aetherfs_btree_node **root,
				   uint64_t ino, uint64_t block,
				   uint32_t size, uint32_t flags)
{
	struct aetherfs_btree_node *node = *root;
	struct aetherfs_btree_key *keys;
	int slot;

	if (!root || !node)
		return -EINVAL;

	btree_write_lock();

	if (btree_is_leaf(node)) {
		if (!btree_is_full(node, AETHERFS_BTREE_DEF_ORDER)) {
			slot = btree_find_slot_ino(node, ino);
			keys = btree_keys(node);

			memmove(&keys[slot + 1], &keys[slot],
			       (le32_to_cpu(node->bn_count) - slot) * sizeof(*keys));

			keys[slot].k_ino = cpu_to_le64(ino);
			keys[slot].k_offset = cpu_to_le64(0);
			keys[slot].k_value = cpu_to_le64(block);
			keys[slot].k_len = cpu_to_le32(size);
			keys[slot].k_flags = cpu_to_le32(flags);
			keys[slot].k_checksum = cpu_to_le32(
				aetherfs_crc32(&keys[slot], sizeof(keys[slot]) - 4));

			node->bn_count = cpu_to_le32(le32_to_cpu(node->bn_count) + 1);
		}
	}

	btree_write_unlock();
	return 0;
}

int aetherfs_btree_lookup_metadata(struct aetherfs_btree_node *root,
				    uint64_t ino, uint64_t *block,
				    uint32_t *size, uint32_t *flags)
{
	struct aetherfs_btree_key key, *keys;
	struct aetherfs_btree_node *node;
	int slot;

	if (!root || !block)
		return -EINVAL;

	key.k_ino = cpu_to_le64(ino);
	key.k_offset = cpu_to_le64(0);

	node = root;
	btree_read_lock();

	while (node) {
		slot = btree_find_slot_ino(node, ino);
		keys = btree_keys(node);

		if (slot < le32_to_cpu(node->bn_count) &&
		    keys[slot].k_ino == key.k_ino) {
			*block = le64_to_cpu(keys[slot].k_value);
			if (size) *size = le32_to_cpu(keys[slot].k_len);
			if (flags) *flags = le32_to_cpu(keys[slot].k_flags);
			btree_read_unlock();
			return 0;
		}

		if (btree_is_leaf(node))
			break;
	}

	btree_read_unlock();
	return -ENOENT;
}

int aetherfs_btree_delete_extent(struct aetherfs_btree_node **root,
				  uint64_t ino, uint64_t lblock)
{
	struct aetherfs_btree_key key, *keys;
	struct aetherfs_btree_node *node;
	int slot;

	if (!root)
		return -EINVAL;

	node = *root;
	if (!node)
		return -ENOENT;

	key.k_ino = cpu_to_le64(ino);
	key.k_offset = cpu_to_le64(lblock);

	btree_write_lock();

	slot = btree_find_slot_extent(node, lblock);
	keys = btree_keys(node);

	if (slot >= le32_to_cpu(node->bn_count) ||
	    keys[slot].k_ino != key.k_ino ||
	    keys[slot].k_offset != key.k_offset) {
		btree_write_unlock();
		return -ENOENT;
	}

	memmove(&keys[slot], &keys[slot + 1],
	       (le32_to_cpu(node->bn_count) - slot - 1) * sizeof(*keys));
	node->bn_count = cpu_to_le32(le32_to_cpu(node->bn_count) - 1);

	btree_write_unlock();
	return 0;
}

static int btree_split_node(struct aetherfs_btree_node *node,
			    struct aetherfs_btree_node **left,
			    struct aetherfs_btree_node **right,
			    uint32_t order)
{
	struct aetherfs_btree_node *l, *r;
	struct aetherfs_btree_key *keys;
	int mid;

	mid = le32_to_cpu(node->bn_count) / 2;

	l = btree_alloc_node(GFP_NOFS);
	r = btree_alloc_node(GFP_NOFS);
	if (!l || !r) {
		btree_free_node(l);
		btree_free_node(r);
		return -ENOMEM;
	}

	l->bn_level = node->bn_level;
	r->bn_level = node->bn_level;

	l->bn_count = cpu_to_le32(mid);
	r->bn_count = cpu_to_le32(le32_to_cpu(node->bn_count) - mid);

	keys = btree_keys(node);
	memcpy(btree_keys(l), keys, mid * sizeof(*keys));
	memcpy(btree_keys(r), &keys[mid],
	       (le32_to_cpu(node->bn_count) - mid) * sizeof(*keys));

	*left = l;
	*right = r;

	return 0;
}

int aetherfs_btree_init(void)
{
	btree_node_cache = kmem_cache_create("aetherfs_btree_node",
					      sizeof(struct aetherfs_btree_node),
					      0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!btree_node_cache)
		return -ENOMEM;

	pr_info("AetherFS: B+tree initialized with order %d\n", AETHERFS_BTREE_DEF_ORDER);
	return 0;
}

void aetherfs_btree_exit(void)
{
	if (btree_node_cache)
		kmem_cache_destroy(btree_node_cache);
}