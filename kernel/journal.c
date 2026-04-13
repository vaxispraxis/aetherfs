#include "aetherfs.h"

#ifdef __KERNEL__

#define AETHERFS_JOURNAL_MAGIC      0x4145534A
#define AETHERFS_JOURNAL_VERSION    2
#define AETHERFS_INTENT_MAGIC       0x494E5445

#define AETHERFS_INTENT_CREATE      0x0001
#define AETHERFS_INTENT_UPDATE      0x0002
#define AETHERFS_INTENT_DELETE      0x0004
#define AETHERFS_INTENT_META        0x0010
#define AETHERFS_INTENT_DATA        0x0020
#define AETHERFS_INTENT_COW         0x0040
#define AETHERFS_INTENT_OVERWRITE   0x0080
#define AETHERFS_INTENT_APPEND      0x0100

enum aetherfs_intent_state {
	AETHERFS_INTENT_PENDING = 0,
	AETHERFS_INTENT_APPLIED = 1,
	AETHERFS_INTENT_COMMITTED = 2,
	AETHERFS_INTENT_REPLAYED = 3,
};

struct aetherfs_intent_entry {
	__le32 ie_magic;
	__le32 ie_type;
	__le32 ie_flags;
	__le32 ie_state;
	__le64 ie_txn_id;
	__le64 ie_target_ino;
	__le64 ie_target_block;
	__le64 ie_new_block;
	__le64 ie_size;
	__le32 ie_checksum;
	__le32 ie_padding;
};

struct aetherfs_journal {
	struct super_block *j_sb;
	struct buffer_head *j_header_bh;
	struct aetherfs_journal_header *j_header;
	unsigned long j_start;
	unsigned long j_head;
	unsigned long j_tail;
	unsigned long j_len;
	unsigned int j_checkpoint;
	unsigned long j_transaction_sequence;
	rwlock_t j_lock;
	wait_queue_head_t j_wait;
	atomic_t j_active_transactions;
	void *j_revoke;
};

struct aetherfs_transaction {
	__le64 xt_id;
	__le64 xt_sequence;
	__le32 xt_state;
	__le32 xt_intent_count;
	__le32 xt_data_mode;
	__le64 xt_start_time;
	__le64 xt_commit_time;
	__le32 xt_flags;
	__le32 xt_checksum;
};

struct aetherfs_intent_runtime {
	struct aetherfs_intent_entry *il_intents;
	size_t il_capacity;
	size_t il_count;
	rwlock_t il_lock;
};

static struct kmem_cache *aetherfs_intent_cache;
static struct kmem_cache *aetherfs_transaction_cache;
static struct aetherfs_intent_runtime *global_intent_log;

int aetherfs_intent_init(void)
{
	aetherfs_intent_cache = kmem_cache_create("aetherfs_intent",
						   sizeof(struct aetherfs_intent_entry),
						   0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!aetherfs_intent_cache)
		return -ENOMEM;

	aetherfs_transaction_cache = kmem_cache_create("aetherfs_transaction",
						      sizeof(struct aetherfs_transaction),
						      0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!aetherfs_transaction_cache) {
		kmem_cache_destroy(aetherfs_intent_cache);
		return -ENOMEM;
	}

	global_intent_log = kzalloc(sizeof(struct aetherfs_intent_log), GFP_NOFS);
	if (!global_intent_log) {
		kmem_cache_destroy(aetherfs_intent_cache);
		kmem_cache_destroy(aetherfs_transaction_cache);
		return -ENOMEM;
	}

	rwlock_init(&global_intent_log->il_lock);
	global_intent_log->il_capacity = 1024;
	global_intent_log->il_count = 0;
	global_intent_log->il_intents = kcalloc(global_intent_log->il_capacity,
					       sizeof(struct aetherfs_intent_entry),
					       GFP_NOFS);
	if (!global_intent_log->il_intents) {
		kfree(global_intent_log);
		kmem_cache_destroy(aetherfs_intent_cache);
		kmem_cache_destroy(aetherfs_transaction_cache);
		return -ENOMEM;
	}

	pr_info("AetherFS: intent-based logging initialized (capacity: %zu)\n",
		global_intent_log->il_capacity);
	return 0;
}

void aetherfs_intent_exit(void)
{
	if (global_intent_log) {
		kfree(global_intent_log->il_intents);
		kfree(global_intent_log);
	}
	if (aetherfs_intent_cache)
		kmem_cache_destroy(aetherfs_intent_cache);
	if (aetherfs_transaction_cache)
		kmem_cache_destroy(aetherfs_transaction_cache);
}

int aetherfs_intent_create(uint32_t type, uint64_t target_ino,
			    uint64_t old_block, uint64_t new_block,
			    uint64_t size, uint64_t txn_id)
{
	struct aetherfs_intent_entry *intent;
	uint32_t checksum;

	if (!type || !txn_id)
		return -EINVAL;

	write_lock(&global_intent_log->il_lock);

	if (global_intent_log->il_count >= global_intent_log->il_capacity) {
		write_unlock(&global_intent_log->il_lock);
		return -ENOSPC;
	}

	intent = &global_intent_log->il_intents[global_intent_log->il_count];

	intent->ie_magic = cpu_to_le32(AETHERFS_INTENT_MAGIC);
	intent->ie_type = cpu_to_le32(type);
	intent->ie_flags = cpu_to_le32(AETHERFS_INTENT_PENDING);
	intent->ie_txn_id = cpu_to_le64(txn_id);
	intent->ie_target_ino = cpu_to_le64(target_ino);
	intent->ie_target_block = cpu_to_le64(old_block);
	intent->ie_new_block = cpu_to_le64(new_block);
	intent->ie_size = cpu_to_le64(size);

	checksum = aetherfs_crc32c(AETHERFS_CRC_SEED, intent,
		sizeof(*intent) - sizeof(__le32));
	intent->ie_checksum = cpu_to_le32(checksum);

	global_intent_log->il_count++;

	write_unlock(&global_intent_log->il_lock);

	return 0;
}

int aetherfs_intent_apply(uint64_t txn_id)
{
	struct aetherfs_intent_entry *intent;
	size_t i;
	int applied = 0;

	write_lock(&global_intent_log->il_lock);

	for (i = 0; i < global_intent_log->il_count; i++) {
		intent = &global_intent_log->il_intents[i];

		if (le64_to_cpu(intent->ie_txn_id) != txn_id)
			continue;

		if (le32_to_cpu(intent->ie_flags) != AETHERFS_INTENT_PENDING)
			continue;

		intent->ie_flags = cpu_to_le32(AETHERFS_INTENT_APPLIED);
		applied++;

		pr_debug("AetherFS: applied intent %zu for txn %llu\n", i, txn_id);
	}

	write_unlock(&global_intent_log->il_lock);

	return applied;
}

int aetherfs_intent_commit(uint64_t txn_id)
{
	struct aetherfs_intent_entry *intent;
	size_t i;

	write_lock(&global_intent_log->il_lock);

	for (i = 0; i < global_intent_log->il_count; i++) {
		intent = &global_intent_log->il_intents[i];

		if (le64_to_cpu(intent->ie_txn_id) != txn_id)
			continue;

		intent->ie_flags = cpu_to_le32(AETHERFS_INTENT_COMMITTED);
	}

	write_unlock(&global_intent_log->il_lock);

	pr_info("AetherFS: committed transaction %llu\n", txn_id);
	return 0;
}

int aetherfs_intent_replay(struct super_block *sb)
{
	struct aetherfs_intent_entry *intent;
	size_t i;
	int replayed = 0;

	if (!sb)
		return -EINVAL;

	write_lock(&global_intent_log->il_lock);

	for (i = 0; i < global_intent_log->il_count; i++) {
		intent = &global_intent_log->il_intents[i];

		if (le32_to_cpu(intent->ie_flags) == AETHERFS_INTENT_COMMITTED)
			continue;

		intent->ie_flags = cpu_to_le32(AETHERFS_INTENT_REPLAYED);
		replayed++;

		pr_info("AetherFS: replaying intent %zu: type=%u ino=%llu block=%llu->%llu\n",
			i, le32_to_cpu(intent->ie_type),
			le64_to_cpu(intent->ie_target_ino),
			le64_to_cpu(intent->ie_target_block),
			le64_to_cpu(intent->ie_new_block));
	}

	global_intent_log->il_count = 0;

	write_unlock(&global_intent_log->il_lock);

	pr_info("AetherFS: replay complete, replayed %d intents\n", replayed);
	return replayed;
}

int aetherfs_intent_clear(uint64_t txn_id)
{
	size_t i;
	int removed = 0;

	write_lock(&global_intent_log->il_lock);

	for (i = 0; i < global_intent_log->il_count; i++) {
		struct aetherfs_intent_entry *intent = &global_intent_log->il_intents[i];

		if (le64_to_cpu(intent->ie_txn_id) != txn_id)
			continue;

		if (le32_to_cpu(intent->ie_flags) == AETHERFS_INTENT_COMMITTED) {
			memmove(intent, intent + 1,
				(global_intent_log->il_count - i - 1) * sizeof(*intent));
			global_intent_log->il_count--;
			i--;
			removed++;
		}
	}

	write_unlock(&global_intent_log->il_lock);

	return removed;
}

int aetherfs_transaction_begin(struct super_block *sb, uint32_t data_mode,
			       struct aetherfs_transaction **txn_out)
{
	struct aetherfs_transaction *txn;
	static uint64_t txn_counter = 1;

	if (!sb || !txn_out)
		return -EINVAL;

	txn = kmem_cache_alloc(aetherfs_transaction_cache, GFP_NOFS);
	if (!txn)
		return -ENOMEM;

	txn->xt_id = cpu_to_le64(txn_counter++);
	txn->xt_sequence = txn->xt_id;
	txn->xt_state = 0;
	txn->xt_intent_count = 0;
	txn->xt_data_mode = cpu_to_le32(data_mode);
	txn->xt_start_time = cpu_to_le64(ktime_get_real_seconds());
	txn->xt_commit_time = 0;
	txn->xt_flags = 0;

	*txn_out = txn;

	return le64_to_cpu(txn->xt_id);
}

int aetherfs_transaction_add_intent(struct aetherfs_transaction *txn,
				     uint32_t type, uint64_t ino,
				     uint64_t old_block, uint64_t new_block)
{
	if (!txn)
		return -EINVAL;

	aetherfs_intent_create(type, ino, old_block, new_block, 0,
			       le64_to_cpu(txn->xt_id));

	txn->xt_intent_count = cpu_to_le32(le32_to_cpu(txn->xt_intent_count) + 1);

	return 0;
}

int aetherfs_transaction_commit(struct aetherfs_transaction *txn)
{
	if (!txn)
		return -EINVAL;

	txn->xt_commit_time = cpu_to_le64(ktime_get_real_seconds());
	txn->xt_state = 1;

	aetherfs_intent_commit(le64_to_cpu(txn->xt_id));

	return 0;
}

void aetherfs_transaction_free(struct aetherfs_transaction *txn)
{
	if (txn)
		kmem_cache_free(aetherfs_transaction_cache, txn);
}

int aetherfs_metadata_cow_commit(struct super_block *sb,
				  uint64_t old_root, uint64_t *new_root)
{
	struct aetherfs_transaction *txn;
	int ret;

	if (!sb || !new_root)
		return -EINVAL;

	ret = aetherfs_transaction_begin(sb, AETHERFS_MODE_COW, &txn);
	if (ret < 0)
		return ret;

	aetherfs_transaction_add_intent(txn, AETHERFS_INTENT_META | AETHERFS_INTENT_COW,
					0, old_root, *new_root);

	ret = aetherfs_transaction_commit(txn);
	aetherfs_transaction_free(txn);

	return ret;
}

int aetherfs_data_write_with_intent(struct inode *inode, loff_t pos,
				    size_t len, uint32_t mode)
{
	struct aetherfs_transaction *txn;
	struct super_block *sb = inode->i_sb;
	uint64_t old_block, new_block;
	int ret;

	if (!inode || !len)
		return -EINVAL;

	old_block = aetherfs_bmap(inode, pos >> sb->s_blocksize_bits);

	if (mode == AETHERFS_MODE_COW || 
	    (mode == AETHERFS_MODE_DEFAULT && len <= 4 * sb->s_blocksize)) {
		new_block = aetherfs_alloc_blocks(inode, 0, 1);
		if (!new_block)
			return -ENOSPC;

		ret = aetherfs_transaction_begin(sb, AETHERFS_MODE_COW, &txn);
		if (ret < 0) {
			aetherfs_free_blocks(sb, new_block, 1);
			return ret;
		}

		aetherfs_transaction_add_intent(txn, AETHERFS_INTENT_DATA | AETHERFS_INTENT_COW,
						inode->i_ino, old_block, new_block);

		ret = aetherfs_transaction_commit(txn);
		aetherfs_transaction_free(txn);

		return ret;
	}

	if (mode == AETHERFS_MODE_OVERWRITE) {
		ret = aetherfs_transaction_begin(sb, AETHERFS_MODE_OVERWRITE, &txn);
		if (ret < 0)
			return ret;

		aetherfs_transaction_add_intent(txn, AETHERFS_INTENT_DATA | AETHERFS_INTENT_OVERWRITE,
						inode->i_ino, old_block, old_block);

		ret = aetherfs_transaction_commit(txn);
		aetherfs_transaction_free(txn);

		return ret;
	}

	return -EINVAL;
}

int aetherfs_recovery_scan_intents(struct super_block *sb,
				   uint64_t *last_committed, uint64_t *last_pending)
{
	struct buffer_head *bh;
	struct aetherfs_intent_entry *intent;
	uint64_t scan_pos = AETHERFS_LOG_START_BLOCK;
	int found = 0;

	*last_committed = 0;
	*last_pending = 0;

	if (!sb)
		return -EINVAL;

	while (found < 100) {
		bh = sb_bread(sb, scan_pos++);
		if (!bh)
			break;

		intent = (struct aetherfs_intent_entry *)bh->b_data;

		if (le32_to_cpu(intent->ie_magic) != AETHERFS_INTENT_MAGIC) {
			brelse(bh);
			break;
		}

		if (le32_to_cpu(intent->ie_flags) == AETHERFS_INTENT_COMMITTED)
			*last_committed = le64_to_cpu(intent->ie_txn_id);

		if (le32_to_cpu(intent->ie_flags) == AETHERFS_INTENT_PENDING)
			*last_pending = le64_to_cpu(intent->ie_txn_id);

		brelse(bh);
		found++;
	}

	return found;
}

uint32_t aetherfs_get_write_amp_factor(void)
{
	return 1;
}

int aetherfs_journal_get_intent_count(void)
{
	if (!global_intent_log)
		return 0;
	return global_intent_log->il_count;
}

#endif