#ifndef KLOCKLAB_H
#define KLOCKLAB_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KLOCKLAB_DEVICE_NAME "klocklab"
#define KLOCKLAB_NUM_KEYS 1024
#define KLOCKLAB_NUM_SHARDS 64
#define KLOCKLAB_MAX_LATENCY_BUCKETS 16
#define KLOCKLAB_HEATMAP_ROWS 64   /* per-CPU rows (max CPUs tracked) */

enum klocklab_mode {
    KLOCKLAB_MODE_GLOBAL  = 0,
    KLOCKLAB_MODE_SHARDED = 1,
    KLOCKLAB_MODE_PERCPU  = 2,
    KLOCKLAB_MODE_RCU     = 3,  /* RCU: near-zero-cost reads, serialized writes */
    KLOCKLAB_MODE_ATOMIC  = 4,  /* Lock-free atomic counters */
};

/* Operation types for read/write mix */
enum klocklab_op_type {
    KLOCKLAB_OP_WRITE = 0,
    KLOCKLAB_OP_READ  = 1,
};

struct klocklab_update_req {
    __u32 key;
    __u32 op_type;  /* 0=write (increment), 1=read (lookup) */
};

struct klocklab_stats {
    __u64 total_updates;
    __u64 total_reads;
    __u64 total_sum;
    __u64 invalid_requests;
    __u32 mode;
    __u32 num_keys;
    __u32 num_shards;
    __u32 nr_cpus_seen;
    __u64 sample[8];
    /* Per-shard contention: ops routed to each shard */
    __u64 shard_hits[KLOCKLAB_NUM_SHARDS];
    /* Latency buckets: bucket[i] = count of ops taking 2^i to 2^(i+1) ns */
    __u64 latency_buckets[KLOCKLAB_MAX_LATENCY_BUCKETS];
    __u64 latency_min_ns;
    __u64 latency_max_ns;
    __u64 latency_total_ns;
};

/*
 * Contention heatmap: per-CPU x per-shard operation counts.
 * heatmap[cpu][shard] = number of ops from that CPU hitting that shard.
 * Allows visualization of which CPUs contend on which shards.
 */
struct klocklab_heatmap {
    __u32 nr_cpus;
    __u32 nr_shards;
    __u64 cells[KLOCKLAB_HEATMAP_ROWS][KLOCKLAB_NUM_SHARDS];
};

#define KLOCKLAB_IOC_MAGIC 'k'
#define KLOCKLAB_IOCTL_UPDATE      _IOW(KLOCKLAB_IOC_MAGIC, 1, struct klocklab_update_req)
#define KLOCKLAB_IOCTL_GET_STATS   _IOR(KLOCKLAB_IOC_MAGIC, 2, struct klocklab_stats)
#define KLOCKLAB_IOCTL_RESET       _IO(KLOCKLAB_IOC_MAGIC,  3)
#define KLOCKLAB_IOCTL_GET_HEATMAP _IO(KLOCKLAB_IOC_MAGIC, 4)

#endif
