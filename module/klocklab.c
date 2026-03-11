// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/preempt.h>
#include <linux/string.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>

#include "../common/klocklab.h"

/* ---- Data structures ---- */

struct klocklab_percpu_bank {
    u64 counters[KLOCKLAB_NUM_KEYS];
};

struct klocklab_shard {
    spinlock_t lock;
    atomic64_t hit_count;
} ____cacheline_aligned;

/* RCU-protected counter array: readers use rcu_read_lock, writers swap */
struct klocklab_rcu_array {
    u64 counters[KLOCKLAB_NUM_KEYS];
    struct rcu_head rcu;
};

/* Per-CPU heatmap: tracks which shard each CPU hits */
struct klocklab_percpu_heatmap {
    u64 shard_ops[KLOCKLAB_NUM_SHARDS];
};

/* ---- Module parameters ---- */

static int mode = KLOCKLAB_MODE_GLOBAL;
module_param(mode, int, 0644);
MODULE_PARM_DESC(mode, "0=global 1=sharded 2=percpu 3=rcu 4=atomic");

/* ---- Global mode ---- */
static u64 *global_counters;
static DEFINE_SPINLOCK(global_lock);

/* ---- Sharded mode ---- */
static u64 *sharded_counters;
static struct klocklab_shard shards[KLOCKLAB_NUM_SHARDS];

/* ---- Per-CPU mode ---- */
static struct klocklab_percpu_bank __percpu *percpu_banks;

/* ---- RCU mode ---- */
static struct klocklab_rcu_array __rcu *rcu_counters;
static DEFINE_MUTEX(rcu_write_mutex);  /* serializes writers */

/* ---- Atomic mode ---- */
static atomic64_t *atomic_counters;  /* array of KLOCKLAB_NUM_KEYS */

/* ---- Shared counters ---- */
static atomic64_t total_updates;
static atomic64_t total_reads;
static atomic64_t invalid_requests;

/* ---- Latency tracking ---- */
static atomic64_t latency_buckets[KLOCKLAB_MAX_LATENCY_BUCKETS];
static atomic64_t latency_min_ns;
static atomic64_t latency_max_ns;
static atomic64_t latency_total_ns;

/* ---- Contention heatmap (per-CPU) ---- */
static struct klocklab_percpu_heatmap __percpu *heatmap_data;

/* ---- debugfs ---- */
static struct dentry *dbgfs_dir;

/* ---- Helpers ---- */

static inline u32 key_to_idx(u32 key)
{
    return key % KLOCKLAB_NUM_KEYS;
}

static inline u32 idx_to_shard(u32 idx)
{
    return idx % KLOCKLAB_NUM_SHARDS;
}

static inline void record_latency(u64 ns)
{
    int bucket;
    u64 old_val;

    atomic64_add(ns, &latency_total_ns);

    bucket = (ns == 0) ? 0 : min_t(int, ilog2(ns), KLOCKLAB_MAX_LATENCY_BUCKETS - 1);
    atomic64_inc(&latency_buckets[bucket]);

    old_val = atomic64_read(&latency_min_ns);
    while (ns < old_val) {
        if (atomic64_cmpxchg(&latency_min_ns, old_val, ns) == old_val)
            break;
        old_val = atomic64_read(&latency_min_ns);
    }

    old_val = atomic64_read(&latency_max_ns);
    while (ns > old_val) {
        if (atomic64_cmpxchg(&latency_max_ns, old_val, ns) == old_val)
            break;
        old_val = atomic64_read(&latency_max_ns);
    }
}

static inline void heatmap_record(u32 shard_id)
{
    struct klocklab_percpu_heatmap *h;

    h = get_cpu_ptr(heatmap_data);
    h->shard_ops[shard_id]++;
    put_cpu_ptr(heatmap_data);
}

/* ---- RCU helpers ---- */

static void rcu_array_free_callback(struct rcu_head *head)
{
    struct klocklab_rcu_array *old = container_of(head, struct klocklab_rcu_array, rcu);
    kfree(old);
}

/* ---- Reset ---- */

static void klocklab_reset_all(void)
{
    int cpu, i;
    unsigned long flags;

    atomic64_set(&total_updates, 0);
    atomic64_set(&total_reads, 0);
    atomic64_set(&invalid_requests, 0);
    atomic64_set(&latency_min_ns, U64_MAX);
    atomic64_set(&latency_max_ns, 0);
    atomic64_set(&latency_total_ns, 0);

    for (i = 0; i < KLOCKLAB_MAX_LATENCY_BUCKETS; i++)
        atomic64_set(&latency_buckets[i], 0);

    /* Reset heatmap */
    if (heatmap_data) {
        for_each_possible_cpu(cpu) {
            struct klocklab_percpu_heatmap *h = per_cpu_ptr(heatmap_data, cpu);
            memset(h->shard_ops, 0, sizeof(h->shard_ops));
        }
    }

    if (global_counters) {
        spin_lock_irqsave(&global_lock, flags);
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            global_counters[i] = 0;
        spin_unlock_irqrestore(&global_lock, flags);
    }

    if (sharded_counters) {
        for (i = 0; i < KLOCKLAB_NUM_SHARDS; i++) {
            spin_lock(&shards[i].lock);
            atomic64_set(&shards[i].hit_count, 0);
        }
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            sharded_counters[i] = 0;
        for (i = KLOCKLAB_NUM_SHARDS - 1; i >= 0; i--)
            spin_unlock(&shards[i].lock);
    }

    if (percpu_banks) {
        for_each_possible_cpu(cpu) {
            struct klocklab_percpu_bank *b = per_cpu_ptr(percpu_banks, cpu);
            memset(b->counters, 0, sizeof(b->counters));
        }
    }

    /* RCU reset: allocate a fresh zeroed array and swap */
    if (rcu_dereference_raw(rcu_counters)) {
        struct klocklab_rcu_array *fresh, *old;

        fresh = kzalloc(sizeof(*fresh), GFP_KERNEL);
        if (fresh) {
            mutex_lock(&rcu_write_mutex);
            old = rcu_dereference_protected(rcu_counters,
                    lockdep_is_held(&rcu_write_mutex));
            rcu_assign_pointer(rcu_counters, fresh);
            mutex_unlock(&rcu_write_mutex);
            if (old)
                call_rcu(&old->rcu, rcu_array_free_callback);
        }
    }

    /* Atomic reset */
    if (atomic_counters) {
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            atomic64_set(&atomic_counters[i], 0);
    }
}

/* ---- Core operation ---- */

static long klocklab_do_op(const struct klocklab_update_req *req)
{
    u32 idx = key_to_idx(req->key);
    u32 shard = idx_to_shard(idx);
    int is_read = (req->op_type == KLOCKLAB_OP_READ);
    ktime_t t_start, t_end;
    u64 elapsed_ns;

    t_start = ktime_get();

    switch (mode) {
    case KLOCKLAB_MODE_GLOBAL: {
        unsigned long flags;
        spin_lock_irqsave(&global_lock, flags);
        if (is_read)
            (void)READ_ONCE(global_counters[idx]);
        else
            global_counters[idx]++;
        spin_unlock_irqrestore(&global_lock, flags);
        heatmap_record(shard);
        break;
    }

    case KLOCKLAB_MODE_SHARDED: {
        unsigned long flags;
        spin_lock_irqsave(&shards[shard].lock, flags);
        if (is_read)
            (void)READ_ONCE(sharded_counters[idx]);
        else
            sharded_counters[idx]++;
        spin_unlock_irqrestore(&shards[shard].lock, flags);
        atomic64_inc(&shards[shard].hit_count);
        heatmap_record(shard);
        break;
    }

    case KLOCKLAB_MODE_PERCPU: {
        struct klocklab_percpu_bank *b = get_cpu_ptr(percpu_banks);
        if (is_read)
            (void)READ_ONCE(b->counters[idx]);
        else
            b->counters[idx]++;
        put_cpu_ptr(percpu_banks);
        heatmap_record(shard);
        break;
    }

    case KLOCKLAB_MODE_RCU: {
        if (is_read) {
            /* RCU read: near-zero overhead */
            struct klocklab_rcu_array *arr;
            rcu_read_lock();
            arr = rcu_dereference(rcu_counters);
            (void)READ_ONCE(arr->counters[idx]);
            rcu_read_unlock();
        } else {
            /* RCU write: copy-on-write under mutex */
            struct klocklab_rcu_array *new_arr, *old_arr;

            new_arr = kmalloc(sizeof(*new_arr), GFP_KERNEL);
            if (!new_arr)
                return -ENOMEM;

            mutex_lock(&rcu_write_mutex);
            old_arr = rcu_dereference_protected(rcu_counters,
                        lockdep_is_held(&rcu_write_mutex));
            memcpy(new_arr->counters, old_arr->counters,
                   sizeof(old_arr->counters));
            new_arr->counters[idx]++;
            rcu_assign_pointer(rcu_counters, new_arr);
            mutex_unlock(&rcu_write_mutex);
            call_rcu(&old_arr->rcu, rcu_array_free_callback);
        }
        heatmap_record(shard);
        break;
    }

    case KLOCKLAB_MODE_ATOMIC: {
        if (is_read)
            (void)atomic64_read(&atomic_counters[idx]);
        else
            atomic64_inc(&atomic_counters[idx]);
        heatmap_record(shard);
        break;
    }

    default:
        atomic64_inc(&invalid_requests);
        return -EINVAL;
    }

    t_end = ktime_get();
    elapsed_ns = ktime_to_ns(ktime_sub(t_end, t_start));
    record_latency(elapsed_ns);

    if (is_read)
        atomic64_inc(&total_reads);
    else
        atomic64_inc(&total_updates);

    return 0;
}

/* ---- Stats collection ---- */

static void klocklab_collect_stats(struct klocklab_stats *st)
{
    int i, j, cpu;
    u64 total_sum = 0;

    memset(st, 0, sizeof(*st));
    st->mode = mode;
    st->num_keys = KLOCKLAB_NUM_KEYS;
    st->num_shards = KLOCKLAB_NUM_SHARDS;
    st->total_updates = atomic64_read(&total_updates);
    st->total_reads = atomic64_read(&total_reads);
    st->invalid_requests = atomic64_read(&invalid_requests);

    st->latency_min_ns = atomic64_read(&latency_min_ns);
    st->latency_max_ns = atomic64_read(&latency_max_ns);
    st->latency_total_ns = atomic64_read(&latency_total_ns);
    if (st->latency_min_ns == U64_MAX)
        st->latency_min_ns = 0;
    for (i = 0; i < KLOCKLAB_MAX_LATENCY_BUCKETS; i++)
        st->latency_buckets[i] = atomic64_read(&latency_buckets[i]);

    for (i = 0; i < KLOCKLAB_NUM_SHARDS; i++)
        st->shard_hits[i] = atomic64_read(&shards[i].hit_count);

    switch (mode) {
    case KLOCKLAB_MODE_GLOBAL: {
        unsigned long flags;
        spin_lock_irqsave(&global_lock, flags);
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            total_sum += global_counters[i];
        for (j = 0; j < 8; j++)
            st->sample[j] = global_counters[j];
        spin_unlock_irqrestore(&global_lock, flags);
        break;
    }

    case KLOCKLAB_MODE_SHARDED: {
        for (i = 0; i < KLOCKLAB_NUM_SHARDS; i++)
            spin_lock(&shards[i].lock);
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            total_sum += sharded_counters[i];
        for (j = 0; j < 8; j++)
            st->sample[j] = sharded_counters[j];
        for (i = KLOCKLAB_NUM_SHARDS - 1; i >= 0; i--)
            spin_unlock(&shards[i].lock);
        break;
    }

    case KLOCKLAB_MODE_PERCPU: {
        st->nr_cpus_seen = num_possible_cpus();
        for_each_possible_cpu(cpu) {
            struct klocklab_percpu_bank *b = per_cpu_ptr(percpu_banks, cpu);
            for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
                total_sum += b->counters[i];
            for (j = 0; j < 8; j++)
                st->sample[j] += b->counters[j];
        }
        break;
    }

    case KLOCKLAB_MODE_RCU: {
        struct klocklab_rcu_array *arr;
        rcu_read_lock();
        arr = rcu_dereference(rcu_counters);
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            total_sum += arr->counters[i];
        for (j = 0; j < 8; j++)
            st->sample[j] = arr->counters[j];
        rcu_read_unlock();
        break;
    }

    case KLOCKLAB_MODE_ATOMIC: {
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            total_sum += atomic64_read(&atomic_counters[i]);
        for (j = 0; j < 8; j++)
            st->sample[j] = atomic64_read(&atomic_counters[j]);
        break;
    }

    default:
        break;
    }

    st->total_sum = total_sum;
}

static void klocklab_collect_heatmap(struct klocklab_heatmap *hm)
{
    int cpu, s, row;

    memset(hm, 0, sizeof(*hm));
    hm->nr_cpus = min_t(u32, num_possible_cpus(), KLOCKLAB_HEATMAP_ROWS);
    hm->nr_shards = KLOCKLAB_NUM_SHARDS;

    row = 0;
    for_each_possible_cpu(cpu) {
        struct klocklab_percpu_heatmap *h;
        if (row >= KLOCKLAB_HEATMAP_ROWS)
            break;
        h = per_cpu_ptr(heatmap_data, cpu);
        for (s = 0; s < KLOCKLAB_NUM_SHARDS; s++)
            hm->cells[row][s] = h->shard_ops[s];
        row++;
    }
}

/* ---- ioctl handler ---- */

static long klocklab_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case KLOCKLAB_IOCTL_UPDATE: {
        struct klocklab_update_req req;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        return klocklab_do_op(&req);
    }

    case KLOCKLAB_IOCTL_GET_STATS: {
        struct klocklab_stats st;
        klocklab_collect_stats(&st);
        if (copy_to_user((void __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        return 0;
    }

    case KLOCKLAB_IOCTL_RESET:
        klocklab_reset_all();
        return 0;

    case KLOCKLAB_IOCTL_GET_HEATMAP: {
        struct klocklab_heatmap *hm;
        /* Large struct — heap allocate */
        hm = kmalloc(sizeof(*hm), GFP_KERNEL);
        if (!hm)
            return -ENOMEM;
        klocklab_collect_heatmap(hm);
        if (copy_to_user((void __user *)arg, hm, sizeof(*hm))) {
            kfree(hm);
            return -EFAULT;
        }
        kfree(hm);
        return 0;
    }

    default:
        atomic64_inc(&invalid_requests);
        return -ENOTTY;
    }
}

static const struct file_operations klocklab_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = klocklab_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = klocklab_ioctl,
#endif
};

static struct miscdevice klocklab_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = KLOCKLAB_DEVICE_NAME,
    .fops  = &klocklab_fops,
    .mode  = 0666,
};

/* ---- debugfs: /sys/kernel/debug/klocklab/ ---- */

static const char * const mode_names[] = {
    "global", "sharded", "percpu", "rcu", "atomic"
};

static int dbgfs_stats_show(struct seq_file *m, void *v)
{
    struct klocklab_stats st;
    int i;
    u64 total_ops;

    klocklab_collect_stats(&st);
    total_ops = st.total_updates + st.total_reads;

    seq_printf(m, "mode            : %s (%u)\n",
               st.mode <= 4 ? mode_names[st.mode] : "unknown", st.mode);
    seq_printf(m, "num_keys        : %u\n", st.num_keys);
    seq_printf(m, "num_shards      : %u\n", st.num_shards);
    seq_printf(m, "nr_cpus_seen    : %u\n", st.nr_cpus_seen);
    seq_printf(m, "total_writes    : %llu\n", st.total_updates);
    seq_printf(m, "total_reads     : %llu\n", st.total_reads);
    seq_printf(m, "total_ops       : %llu\n", total_ops);
    seq_printf(m, "total_sum       : %llu\n", st.total_sum);
    seq_printf(m, "invalid_requests: %llu\n", st.invalid_requests);
    seq_printf(m, "\n");

    seq_printf(m, "latency_min_ns  : %llu\n", st.latency_min_ns);
    seq_printf(m, "latency_max_ns  : %llu\n", st.latency_max_ns);
    if (total_ops > 0)
        seq_printf(m, "latency_avg_ns  : %llu\n", st.latency_total_ns / total_ops);
    seq_printf(m, "\nlatency_histogram (bucket = 2^i ns):\n");
    for (i = 0; i < KLOCKLAB_MAX_LATENCY_BUCKETS; i++) {
        if (st.latency_buckets[i] > 0)
            seq_printf(m, "  [2^%d .. 2^%d) ns : %llu\n",
                       i, i + 1, st.latency_buckets[i]);
    }

    if (st.mode == KLOCKLAB_MODE_SHARDED) {
        seq_printf(m, "\nshard_hits:\n");
        for (i = 0; i < KLOCKLAB_NUM_SHARDS; i++) {
            if (st.shard_hits[i] > 0)
                seq_printf(m, "  shard[%2d] : %llu\n", i, st.shard_hits[i]);
        }
    }

    seq_printf(m, "\nsample[0..7]    : ");
    for (i = 0; i < 8; i++)
        seq_printf(m, "%llu%s", st.sample[i], (i == 7) ? "\n" : ", ");

    return 0;
}

static int dbgfs_stats_open(struct inode *inode, struct file *file)
{
    return single_open(file, dbgfs_stats_show, NULL);
}

static const struct file_operations dbgfs_stats_fops = {
    .owner   = THIS_MODULE,
    .open    = dbgfs_stats_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* debugfs heatmap: CSV-style output for easy parsing */
static int dbgfs_heatmap_show(struct seq_file *m, void *v)
{
    int cpu, s, row;
    u32 nr_cpus = min_t(u32, num_possible_cpus(), KLOCKLAB_HEATMAP_ROWS);

    seq_printf(m, "cpu");
    for (s = 0; s < KLOCKLAB_NUM_SHARDS; s++)
        seq_printf(m, ",shard%d", s);
    seq_printf(m, "\n");

    row = 0;
    for_each_possible_cpu(cpu) {
        struct klocklab_percpu_heatmap *h;
        if (row >= (int)nr_cpus)
            break;
        h = per_cpu_ptr(heatmap_data, cpu);
        seq_printf(m, "%d", cpu);
        for (s = 0; s < KLOCKLAB_NUM_SHARDS; s++)
            seq_printf(m, ",%llu", h->shard_ops[s]);
        seq_printf(m, "\n");
        row++;
    }
    return 0;
}

static int dbgfs_heatmap_open(struct inode *inode, struct file *file)
{
    return single_open(file, dbgfs_heatmap_show, NULL);
}

static const struct file_operations dbgfs_heatmap_fops = {
    .owner   = THIS_MODULE,
    .open    = dbgfs_heatmap_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

static void klocklab_debugfs_init(void)
{
    dbgfs_dir = debugfs_create_dir("klocklab", NULL);
    if (IS_ERR_OR_NULL(dbgfs_dir)) {
        pr_warn("klocklab: debugfs not available\n");
        dbgfs_dir = NULL;
        return;
    }
    debugfs_create_file("stats", 0444, dbgfs_dir, NULL, &dbgfs_stats_fops);
    debugfs_create_file("heatmap", 0444, dbgfs_dir, NULL, &dbgfs_heatmap_fops);
}

static void klocklab_debugfs_exit(void)
{
    debugfs_remove_recursive(dbgfs_dir);
}

/* ---- Module init/exit ---- */

static void klocklab_free_all(void)
{
    struct klocklab_rcu_array *arr;

    kfree(global_counters);
    global_counters = NULL;
    kfree(sharded_counters);
    sharded_counters = NULL;
    if (percpu_banks) {
        free_percpu(percpu_banks);
        percpu_banks = NULL;
    }
    arr = rcu_dereference_raw(rcu_counters);
    if (arr) {
        rcu_assign_pointer(rcu_counters, NULL);
        synchronize_rcu();
        kfree(arr);
    }
    kfree(atomic_counters);
    atomic_counters = NULL;
    if (heatmap_data) {
        free_percpu(heatmap_data);
        heatmap_data = NULL;
    }
}

static int __init klocklab_init(void)
{
    int ret, i;

    if (mode < KLOCKLAB_MODE_GLOBAL || mode > KLOCKLAB_MODE_ATOMIC) {
        pr_err("klocklab: invalid mode=%d\n", mode);
        return -EINVAL;
    }

    atomic64_set(&total_updates, 0);
    atomic64_set(&total_reads, 0);
    atomic64_set(&invalid_requests, 0);
    atomic64_set(&latency_min_ns, U64_MAX);
    atomic64_set(&latency_max_ns, 0);
    atomic64_set(&latency_total_ns, 0);
    for (i = 0; i < KLOCKLAB_MAX_LATENCY_BUCKETS; i++)
        atomic64_set(&latency_buckets[i], 0);

    /* Allocate per-CPU heatmap for all modes */
    heatmap_data = alloc_percpu(struct klocklab_percpu_heatmap);
    if (!heatmap_data)
        return -ENOMEM;
    for_each_possible_cpu(i) {
        struct klocklab_percpu_heatmap *h = per_cpu_ptr(heatmap_data, i);
        memset(h->shard_ops, 0, sizeof(h->shard_ops));
    }

    switch (mode) {
    case KLOCKLAB_MODE_GLOBAL:
        global_counters = kcalloc(KLOCKLAB_NUM_KEYS,
                                  sizeof(*global_counters), GFP_KERNEL);
        if (!global_counters) { klocklab_free_all(); return -ENOMEM; }
        spin_lock_init(&global_lock);
        break;

    case KLOCKLAB_MODE_SHARDED:
        sharded_counters = kcalloc(KLOCKLAB_NUM_KEYS,
                                   sizeof(*sharded_counters), GFP_KERNEL);
        if (!sharded_counters) { klocklab_free_all(); return -ENOMEM; }
        for (i = 0; i < KLOCKLAB_NUM_SHARDS; i++) {
            spin_lock_init(&shards[i].lock);
            atomic64_set(&shards[i].hit_count, 0);
        }
        break;

    case KLOCKLAB_MODE_PERCPU:
        percpu_banks = alloc_percpu(struct klocklab_percpu_bank);
        if (!percpu_banks) { klocklab_free_all(); return -ENOMEM; }
        for_each_possible_cpu(i) {
            struct klocklab_percpu_bank *b = per_cpu_ptr(percpu_banks, i);
            memset(b->counters, 0, sizeof(b->counters));
        }
        break;

    case KLOCKLAB_MODE_RCU: {
        struct klocklab_rcu_array *arr;
        arr = kzalloc(sizeof(*arr), GFP_KERNEL);
        if (!arr) { klocklab_free_all(); return -ENOMEM; }
        rcu_assign_pointer(rcu_counters, arr);
        mutex_init(&rcu_write_mutex);
        break;
    }

    case KLOCKLAB_MODE_ATOMIC:
        atomic_counters = kcalloc(KLOCKLAB_NUM_KEYS,
                                  sizeof(*atomic_counters), GFP_KERNEL);
        if (!atomic_counters) { klocklab_free_all(); return -ENOMEM; }
        for (i = 0; i < KLOCKLAB_NUM_KEYS; i++)
            atomic64_set(&atomic_counters[i], 0);
        break;
    }

    ret = misc_register(&klocklab_miscdev);
    if (ret) {
        pr_err("klocklab: misc_register failed: %d\n", ret);
        klocklab_free_all();
        return ret;
    }

    klocklab_debugfs_init();

    pr_info("klocklab: loaded /dev/%s mode=%d (%s)\n",
            KLOCKLAB_DEVICE_NAME, mode,
            mode <= 4 ? mode_names[mode] : "unknown");
    return 0;
}

static void __exit klocklab_exit(void)
{
    klocklab_debugfs_exit();
    misc_deregister(&klocklab_miscdev);
    klocklab_free_all();
    pr_info("klocklab: unloaded\n");
}

module_init(klocklab_init);
module_exit(klocklab_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI + User");
MODULE_DESCRIPTION("Kernel lock contention lab: global/sharded/percpu/rcu/atomic");
MODULE_VERSION("3.0");
