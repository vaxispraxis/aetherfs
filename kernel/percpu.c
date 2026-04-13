#include "aetherfs.h"
#include <linux/numa.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>

#define AETHERFS_MAX_LANES    64
#define AETHERFS_PERCPU_BATCH 32
#define AETHERFS_NUMA_MAX_NODES 8
#define AETHERFS_RCU_GC_DELAY  (5 * HZ)

enum aetherfs_lane_state {
	LANE_IDLE = 0,
	LANE_ACTIVE = 1,
	LANE_COMMITTING = 2,
	LANE_WAITING = 3,
};

struct aetherfs_percpu_lane {
	atomic_t ll_state;
	uint64_t ll_sequence;
	uint64_t ll_committed_seq;
	void *ll_journal_buf;
	size_t ll_journal_size;
	struct work_struct ll_commit_work;
	atomic_t ll_pending_ops;
};

struct aetherfs_percpu_data {
	atomic_t pd_active;
	rwlock_t pd_lock;
	unsigned long pd_free_head;
	unsigned long pd_free_count;
	unsigned long pd_free_list[64];
	uint32_t pd_journal_seq;
	uint32_t pd_padding[8];
	atomic_t pd_allocation_count;
	atomic_t pd_lock_contention;
};

struct aetherfs_numa_info {
	int ni_node;
	int ni_ncpus;
	void *ni_cached_metadata;
	size_t ni_cache_size;
	rwlock_t ni_cache_lock;
	struct list_head ni_cache_list;
	atomic_t ni_refs;
};

struct aetherfs_parallel_dir {
	struct inode *pd_inode;
	unsigned int pd_cpu;
	struct work_struct pd_work;
	atomic_t pd_in_progress;
};

static DEFINE_PER_CPU(struct aetherfs_percpu_data, aetherfs_percpu_data);
static struct aetherfs_percpu_lane aetherfs_lanes[AETHERFS_MAX_LANES];
static struct aetherfs_numa_info aetherfs_numa_info[AETHERFS_NUMA_MAX_NODES];
static int aetherfs_nr_lanes;
static rwlock_t global_lane_lock = __RW_LOCK_UNLOCKED(global_lane_lock);

int aetherfs_percpu_init(void)
{
	struct aetherfs_percpu_data *p;
	struct aetherfs_percpu_lane *l;
	int cpu, node;

	aetherfs_nr_lanes = min(num_online_cpus(), AETHERFS_MAX_LANES);
	if (aetherfs_nr_lanes < 1)
		aetherfs_nr_lanes = 1;

	for_each_possible_cpu(cpu) {
		p = &per_cpu(aetherfs_percpu_data, cpu);
		rwlock_init(&p->pd_lock);
		atomic_set(&p->pd_active, 0);
		atomic_set(&p->pd_allocation_count, 0);
		atomic_set(&p->pd_lock_contention, 0);
		p->pd_free_count = 0;
		p->pd_journal_seq = 0;
	}

	for (int i = 0; i < aetherfs_nr_lanes; i++) {
		l = &aetherfs_lanes[i];
		atomic_set(&l->ll_state, LANE_IDLE);
		l->ll_sequence = 0;
		l->ll_committed_seq = 0;
		atomic_set(&l->ll_pending_ops, 0);
	}

	for_each_online_node(node) {
		if (node < AETHERFS_NUMA_MAX_NODES) {
			struct aetherfs_numa_info *ni = &aetherfs_numa_info[node];
			ni->ni_node = node;
			ni->ni_ncpus = num_possible_cpus();
			rwlock_init(&ni->ni_cache_lock);
			INIT_LIST_HEAD(&ni->ni_cache_list);
			atomic_set(&ni->ni_refs, 0);
		}
	}

	pr_info("AetherFS: percpu init: %d lanes, %d nodes\n", 
		aetherfs_nr_lanes, nr_online_nodes);
	return 0;
}

struct aetherfs_percpu_data *aetherfs_get_percpu(void)
{
	struct aetherfs_percpu_data *p = this_cpu_ptr(&aetherfs_percpu_data);
	atomic_inc(&p->pd_active);
	return p;
}

void aetherfs_put_percpu(struct aetherfs_percpu_data *p)
{
	if (p)
		atomic_dec(&p->pd_active);
}

int aetherfs_percpu_alloc_block(unsigned long *block)
{
	struct aetherfs_percpu_data *p;
	unsigned long blk;

	preempt_disable();
	p = this_cpu_ptr(&aetherfs_percpu_data);

	if (rwlock_can_lock(&p->pd_lock)) {
		write_lock(&p->pd_lock);
	} else {
		atomic_inc(&p->pd_lock_contention);
		write_lock(&p->pd_lock);
	}

	if (p->pd_free_count > 0) {
		p->pd_free_count--;
		*block = p->pd_free_list[p->pd_free_count];
		write_unlock(&p->pd_lock);
		preempt_enable();
		atomic_dec(&p->pd_allocation_count);
		return 0;
	}

	write_unlock(&p->pd_lock);

	blk = aetherfs_alloc_blocks(NULL, 0, 64);
	if ((long)blk < 0) {
		preempt_enable();
		return (int)blk;
	}

	write_lock(&p->pd_lock);
	p->pd_free_count = 63;
	write_unlock(&p->pd_lock);

	preempt_enable();
	atomic_dec(&p->pd_allocation_count);
	*block = blk;
	return 0;
}

void aetherfs_percpu_free_block(unsigned long block)
{
	struct aetherfs_percpu_data *p;

	preempt_disable();
	p = this_cpu_ptr(&aetherfs_percpu_data);

	write_lock(&p->pd_lock);

	if (p->pd_free_count < 64) {
		p->pd_free_list[p->pd_free_count] = block;
		p->pd_free_count++;
		write_unlock(&p->pd_lock);
	} else {
		write_unlock(&p->pd_lock);
		aetherfs_free_blocks(NULL, block, 1);
	}

	preempt_enable();
}

int aetherfs_journal_percpu_start(void)
{
	struct aetherfs_percpu_data *p;
	struct aetherfs_percpu_lane *l;
	int lane;
	uint64_t seq;

	p = this_cpu_ptr(&aetherfs_percpu_data);
	lane = smp_processor_id() % aetherfs_nr_lanes;
	l = &aetherfs_lanes[lane];

	while (atomic_read(&l->ll_state) == LANE_COMMITTING) {
		cpu_relax();
	}

	seq = l->ll_sequence++;
	atomic_set(&l->ll_state, LANE_ACTIVE);
	atomic_inc(&l->ll_pending_ops);

	p->pd_journal_seq = seq;

	return lane;
}

void aetherfs_journal_percpu_end(int lane)
{
	struct aetherfs_percpu_lane *l;

	if (lane < 0 || lane >= aetherfs_nr_lanes)
		return;

	l = &aetherfs_lanes[lane];
	atomic_dec(&l->ll_pending_ops);

	if (atomic_read(&l->ll_pending_ops) == 0) {
		atomic_set(&l->ll_state, LANE_COMMITTING);
	}
}

int aetherfs_journal_percpu_commit(int lane)
{
	struct aetherfs_percpu_lane *l;
	int retries = 0;

	if (lane < 0 || lane >= aetherfs_nr_lanes)
		return -EINVAL;

	l = &aetherfs_lanes[lane];

	while (atomic_read(&l->ll_pending_ops) > 0) {
		cpu_relax();
		retries++;
		if (retries > 100000)
			return -EAGAIN;
	}

	l->ll_committed_seq = l->ll_sequence;
	atomic_set(&l->ll_state, LANE_IDLE);

	return 0;
}

int aetherfs_get_lane_for_cpu(int cpu)
{
	return cpu % aetherfs_nr_lanes;
}

void aetherfs_percpu_flush(void)
{
	struct aetherfs_percpu_data *p;
	int cpu;

	for_each_possible_cpu(cpu) {
		p = &per_cpu(aetherfs_percpu_data, cpu);

		write_lock(&p->pd_lock);
		while (p->pd_free_count > 0) {
			p->pd_free_count--;
			aetherfs_free_blocks(NULL, p->pd_free_list[p->pd_free_count], 1);
		}
		write_unlock(&p->pd_lock);
	}
}

int aetherfs_numa_init(void)
{
	int node;

	for_each_online_node(node) {
		if (node < AETHERFS_NUMA_MAX_NODES) {
			pr_info("AetherFS: NUMA node %d initialized\n", node);
		}
	}

	return 0;
}

int aetherfs_numa_get_node(void)
{
	int node = numa_node_id();
	if (node >= AETHERFS_NUMA_MAX_NODES)
		node = 0;
	return node;
}

void *aetherfs_numa_alloc_cache(size_t size)
{
	struct aetherfs_numa_info *ni;
	void *cache;

	ni = &aetherfs_numa_info[aetherfs_numa_get_node()];

	cache = kmalloc_node(size, GFP_NOFS, ni->ni_node);
	if (cache) {
		atomic_inc(&ni->ni_refs);
		ni->ni_cache_size += size;
	}

	return cache;
}

void aetherfs_numa_free_cache(void *cache, size_t size)
{
	struct aetherfs_numa_info *ni;
	int node;

	if (!cache)
		return;

	node = numa_node_id();
	if (node >= AETHERFS_NUMA_MAX_NODES)
		node = 0;

	ni = &aetherfs_numa_info[node];
	atomic_dec(&ni->ni_refs);
	ni->ni_cache_size -= size;
	kfree(cache);
}

int aetherfs_numa_cached_meta(struct super_block *sb, void *data, size_t size)
{
	struct aetherfs_numa_info *ni;
	struct aetherfs_numa_info *cached;
	void *buf;

	ni = &aetherfs_numa_info[aetherfs_numa_get_node()];

	buf = aetherfs_numa_alloc_cache(size);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, data, size);

	write_lock(&ni->ni_cache_lock);
	list_add(&((struct list_head *)buf)[0], &ni->ni_cache_list);
	write_unlock(&ni->ni_cache_lock);

	return 0;
}

int aetherfs_parallel_dir_init(struct inode *dir, struct aetherfs_parallel_dir *pdir)
{
	if (!dir || !pdir)
		return -EINVAL;

	pdir->pd_inode = dir;
	pdir->pd_cpu = smp_processor_id();
	atomic_set(&pdir->pd_in_progress, 0);

	return 0;
}

void aetherfs_parallel_dir_schedule(struct aetherfs_parallel_dir *pdir,
				     void (*work_fn)(struct work_struct *))
{
	if (!pdir || !work_fn)
		return;

	INIT_WORK(&pdir->pd_work, work_fn);
	queue_work_on(pdir->pd_cpu, system_unbound_wq, &pdir->pd_work);
}

int aetherfs_parallel_dir_wait(struct aetherfs_parallel_dir *pdir)
{
	if (!pdir)
		return -EINVAL;

	while (atomic_read(&pdir->pd_in_progress) > 0) {
		cpu_relax();
	}

	flush_work(&pdir->pd_work);
	return 0;
}

int aetherfs_get_cpu_locality(void)
{
	return raw_smp_processor_id();
}

int aetherfs_bind_to_node(int node)
{
	if (node < 0 || node >= nr_online_nodes())
		return -EINVAL;

	return set_cpus_allowed_ptr(current, cpumask_of_node(node));
}

uint64_t aetherfs_alloc_local(struct inode *inode, uint32_t count)
{
	int cpu = aetherfs_get_cpu_locality();
	int ag = cpu % AETHERFS_MAX_AG;

	return aetherfs_alloc_blocks_extended(inode->i_sb, count, 
					      AETHERFS_ALLOC_HOT, ag);
}

void aetherfs_percpu_exit(void)
{
	struct aetherfs_numa_info *ni;
	int node;

	aetherfs_percpu_flush();

	for_each_online_node(node) {
		if (node < AETHERFS_NUMA_MAX_NODES) {
			ni = &aetherfs_numa_info[node];
			pr_info("AetherFS: NUMA node %d cache: %zu bytes, %d refs\n",
				node, ni->ni_cache_size, 
				atomic_read(&ni->ni_refs));
		}
	}
}

int aetherfs_rcu_read_lock(void)
{
	rcu_read_lock();
	return 0;
}

void aetherfs_rcu_read_unlock(void)
{
	rcu_read_unlock();
}

void *aetherfs_rcu_dereference(void *p)
{
	return rcu_dereference(p);
}

void aetherfs_rcu_assign_pointer(void *p, void *v)
{
	rcu_assign_pointer(p, v);
}

void aetherfs_call_rcu(void *head, void (*func)(void *))
{
	call_rcu(head, func);
}

int aetherfs_get_alloc_contention(void)
{
	struct aetherfs_percpu_data *p;
	int total = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		p = &per_cpu(aetherfs_percpu_data, cpu);
		total += atomic_read(&p->pd_lock_contention);
	}

	return total;
}

int aetherfs_get_active_operations(void)
{
	struct aetherfs_percpu_data *p;
	int total = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		p = &per_cpu(aetherfs_percpu_data, cpu);
		total += atomic_read(&p->pd_active);
	}

	return total;
}