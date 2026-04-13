#include "aetherfs.h"
#include <linux/errno.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>

struct aetherfs_journal_transaction {
	uint64_t t_txn_id;
	uint32_t t_state;
#define TXN_RUNNING  1
#define TXN_COMMITTING 2
#define TXN_COMMITTED 3
#define TXN_ABORTED  4
	uint64_t t_start;
	uint64_t t_end;
	struct buffer_head *t_buffers;
	uint32_t t_buffer_count;
	uint64_t t_checksum;
};

static struct aetherfs_journal_transaction *aetherfs_journal_current(struct aetherfs_journal_log *log)
{
	if (!log)
		return NULL;
	return NULL;
}

int aetherfs_journal_start(struct aetherfs_journal_log *log, int nblocks)
{
	struct aetherfs_journal_transaction *txn;
	uint64_t head;

	if (!log || nblocks <= 0)
		return -EINVAL;

	txn = kzalloc(sizeof(struct aetherfs_journal_transaction), GFP_NOFS);
	if (!txn)
		return -ENOMEM;

	txn->t_state = TXN_RUNNING;
	txn->t_txn_id = le64_to_cpu(log->j_commit) + 1;
	txn->t_buffer_count = 0;
	txn->t_buffers = NULL;

	head = le64_to_cpu(log->j_head);
	txn->t_start = head;
	txn->t_end = (head + nblocks) % le64_to_cpu(log->j_end);

	log->j_head = cpu_to_le64(txn->t_end);

	return (int)txn->t_txn_id;
}

int aetherfs_journal_commit(struct aetherfs_journal_log *log, uint64_t txn_id)
{
	struct aetherfs_journal_transaction *txn;
	uint32_t checksum;

	if (!log)
		return -EINVAL;

	txn = aetherfs_journal_current(log);
	if (!txn || txn->t_txn_id != txn_id)
		return -EINVAL;

	txn->t_state = TXN_COMMITTING;

	checksum = aetherfs_crc32(&txn->t_txn_id, sizeof(txn->t_txn_id));
	checksum = aetherfs_crc32_extend(checksum, &txn->t_start, sizeof(txn->t_start));
	txn->t_checksum = checksum;

	log->j_commit = cpu_to_le64(txn->t_end);

	txn->t_state = TXN_COMMITTED;
	kfree(txn);

	return 0;
}

int aetherfs_journal_replay(struct aetherfs_journal_log *log)
{
	uint64_t head, commit, pos;
	struct buffer_head *bh;
	uint32_t checksum;

	if (!log)
		return -EINVAL;

	head = le64_to_cpu(log->j_head);
	commit = le64_to_cpu(log->j_commit);

	if (head == commit)
		return 0;

	log->j_flags |= cpu_to_le32(AETHERFS_JOURNAL_RECOVERING);

	pos = commit;
	while (pos != head) {
		bh = sb_bread(log->j_sb, pos);
		if (!bh)
			return -EIO;

		if (!buffer_uptodate(bh)) {
			brelse(bh);
			break;
		}

		checksum = aetherfs_crc32(bh->b_data, log->j_sb->s_blocksize);
		if (checksum) {
			brelse(bh);
			pos = (pos + 1) % le64_to_cpu(log->j_end);
			continue;
		}

		brelse(bh);
		pos = (pos + 1) % le64_to_cpu(log->j_end);
	}

	log->j_head = cpu_to_le64(pos);
	log->j_commit = cpu_to_le64(pos);
	log->j_flags &= cpu_to_le32(~AETHERFS_JOURNAL_RECOVERING);

	return 0;
}

int aetherfs_journal_append(struct aetherfs_journal_log *log,
			  uint64_t txn_id, struct buffer_head *bh,
			  uint32_t op, uint32_t flags)
{
	struct aetherfs_journal_entry *entry;
	uint64_t pos;
	uint32_t checksum;

	if (!log || !bh)
		return -EINVAL;

	pos = le64_to_cpu(log->j_head);

	entry = (struct aetherfs_journal_entry *)bh->b_data;
	entry->je_txn_id = cpu_to_le64(txn_id);
	entry->je_op = cpu_to_le32(op);
	entry->je_flags = cpu_to_le32(flags);
	entry->je_blocknr = cpu_to_le64(pos);

	checksum = aetherfs_crc32(entry, sizeof(*entry) - 4);
	entry->je_checksum = cpu_to_le32(checksum);

	log->j_head = cpu_to_le64((pos + 1) % le64_to_cpu(log->j_end));

	return 0;
}

int aetherfs_journal_checkpointer(struct aetherfs_journal_log *log)
{
	uint64_t checkpoint;
	struct buffer_head *bh;

	if (!log)
		return -EINVAL;

	checkpoint = le64_to_cpu(log->j_commit);
	bh = sb_bread(log->j_sb, le64_to_cpu(log->j_start));
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, log->j_sb->s_blocksize);
	*((uint64_t *)bh->b_data) = cpu_to_le64(checkpoint);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}