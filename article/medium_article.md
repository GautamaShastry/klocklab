# I Built a Linux Kernel Module to Measure Why Your Database Slows Down Under Load

Lock contention is the silent performance killer hiding inside every multi-core system. The Linux kernel spent over a decade removing its Big Kernel Lock. MySQL's global mutex was a known scalability wall for years. Google engineers are still submitting patches in 2024 to break up coarse locks in the kernel's memory manager.

I wanted to see this problem for myself — not in theory, not in a textbook, but measured at the kernel level with real numbers. So I built **klocklab**: a Linux kernel module that implements five different synchronization strategies and benchmarks them under realistic workloads.

Here's what the data shows.

---

## The Problem

Modern CPUs have many cores. Modern software uses many threads. When those threads need to access shared data, they need some form of synchronization to avoid corrupting it.

The simplest approach is a lock: before touching shared data, acquire the lock. When you're done, release it. If another thread wants the same data, it waits.

This works fine with one or two threads. But as you add more cores, something breaks. Threads spend more time waiting for the lock than doing actual work. Your 16-core server performs like a single-core machine. You're paying for hardware you can't use.

This isn't a theoretical problem. It shows up everywhere:

- **Databases**: MySQL's InnoDB had a single `kernel_mutex` that killed performance beyond 8 cores. It took years of engineering to replace it.
- **Operating systems**: Early Linux had the Big Kernel Lock (BKL) — one lock for the entire kernel. Removing it was a decade-long effort spanning 2000 to 2011.
- **Networking**: The Linux networking stack moved from a single receive lock to per-CPU queues to handle 10Gbps+ traffic.
- **Memory allocators**: glibc's malloc used a single arena lock. Google built tcmalloc and Facebook built jemalloc specifically to solve this.

The question isn't whether lock contention matters. It's which solution fits your workload.

---

## The Experiment

I built a kernel module called **klocklab** that creates a device at `/dev/klocklab`. It manages an array of 1024 counters that threads can increment or read through ioctl system calls. The module supports five synchronization modes, selectable at load time:

```bash
# Load with global lock
sudo insmod klocklab.ko mode=0

# Load with sharded locks
sudo insmod klocklab.ko mode=1

# Load with per-CPU counters
sudo insmod klocklab.ko mode=2

# Load with RCU (Read-Copy-Update)
sudo insmod klocklab.ko mode=3

# Load with lock-free atomics
sudo insmod klocklab.ko mode=4
```

A userspace benchmark program spawns multiple threads that hammer the device with operations. Each thread picks a key, sends an ioctl to the kernel, and the kernel increments or reads the corresponding counter using whatever synchronization mode is active.

The benchmark measures throughput (operations per second), per-operation latency, and CPU utilization. The kernel module tracks its own latency histogram and per-shard contention data, exposed through debugfs at `/sys/kernel/debug/klocklab/stats`.

---

## The Five Strategies

### Mode 0: Global Spinlock

The simplest approach. One lock protects all 1024 counters.

```c
spin_lock_irqsave(&global_lock, flags);
global_counters[idx]++;
spin_unlock_irqrestore(&global_lock, flags);
```

Every thread, regardless of which key it wants, must acquire this single lock. If 16 threads are running, 15 of them are spinning at any given moment. This is the Big Kernel Lock problem in miniature.

### Mode 1: Sharded Spinlocks

Split the 1024 keys into 64 shards, each with its own lock. Key `k` maps to shard `k % 64`.

```c
u32 shard = idx % KLOCKLAB_NUM_SHARDS;
spin_lock_irqsave(&shards[shard].lock, flags);
sharded_counters[idx]++;
spin_unlock_irqrestore(&shards[shard].lock, flags);
```

Now threads only block each other if they happen to hit the same shard. With uniform key distribution, contention drops by up to 64x. Each shard struct is cache-line aligned (`____cacheline_aligned`) to prevent false sharing — where two locks on the same 64-byte cache line cause unnecessary cache invalidation between cores.

### Mode 2: Per-CPU Counters

Each CPU gets its own private copy of all 1024 counters. No lock needed — just disable preemption so the thread stays on its CPU during the update.

```c
struct klocklab_percpu_bank *b = get_cpu_ptr(percpu_banks);
b->counters[idx]++;
put_cpu_ptr(percpu_banks);
```

Writes are essentially free. The tradeoff: reading the total requires aggregating across all CPUs, which is expensive. This is the pattern Linux uses for `/proc/stat` CPU counters and network packet statistics.

### Mode 3: RCU (Read-Copy-Update)

The Linux kernel's most sophisticated synchronization mechanism. Reads are nearly free — just `rcu_read_lock()` and `rcu_read_unlock()`, which are essentially no-ops on modern kernels. Writers pay the cost: they copy the entire data structure, modify the copy, swap the pointer, and defer freeing the old copy until all readers are done.

```c
/* Read path — near zero overhead */
rcu_read_lock();
arr = rcu_dereference(rcu_counters);
value = READ_ONCE(arr->counters[idx]);
rcu_read_unlock();

/* Write path — expensive copy-on-write */
new_arr = kmalloc(sizeof(*new_arr), GFP_KERNEL);
mutex_lock(&rcu_write_mutex);
old_arr = rcu_dereference_protected(rcu_counters, ...);
memcpy(new_arr->counters, old_arr->counters, ...);
new_arr->counters[idx]++;
rcu_assign_pointer(rcu_counters, new_arr);
mutex_unlock(&rcu_write_mutex);
call_rcu(&old_arr->rcu, free_callback);
```

RCU is designed for read-heavy workloads. The Linux kernel uses it for routing tables, firewall rules, and module lists — data that's read millions of times per second but rarely modified.

### Mode 4: Lock-Free Atomics

No lock at all. Each counter is an `atomic64_t`, and the CPU hardware handles synchronization through cache coherency protocols (MESI).

```c
atomic64_inc(&atomic_counters[idx]);
```

This is the theoretical performance ceiling for simple counter workloads. The CPU's compare-and-swap instructions handle everything. No spinning, no sleeping, no copying.

---

## The Workloads

Benchmarking with uniform random keys is easy but unrealistic. Real systems have skewed access patterns. I tested three distributions:

**Uniform**: Every key has equal probability. Best case for sharding — load spreads evenly.

**Hotspot**: 90% of operations hit key 0, 10% spread randomly. Worst case for sharding — one shard gets hammered. This simulates a viral event: one database row getting all the traffic.

**Zipfian** (α=1.2): A few keys are very popular, most are rarely accessed. The probability of accessing key `k` is proportional to `1/k^1.2`. This is how real databases, caches, and web traffic actually behave. It's the same distribution used in YCSB (Yahoo Cloud Serving Benchmark), the industry standard for database testing.

I also tested a read-heavy variant (80% reads, 20% writes) and a burst pattern (1000 ops, then 50μs pause) to simulate real-world traffic spikes.

---

## The Results

### Throughput: Global Lock Flatlines, Per-CPU Scales

Under the hotspot workload (write-only, 90% hitting one key):

| Threads | Global | Sharded | Per-CPU | RCU | Atomic |
|---------|--------|---------|---------|-----|--------|
| 1 | 4.3M | 1.2M | 0.7M | 0.4M | 3.2M |
| 4 | 2.7M | 2.6M | 4.3M | 0.5M | 5.5M |
| 8 | 3.9M | 4.5M | 6.4M | 0.1M | 4.1M |
| 16 | 3.6M | 4.3M | 7.4M | 0.4M | 6.8M |

The global lock stays flat around 3.5-4M ops/sec regardless of thread count. Adding more threads doesn't help — they just spin. Per-CPU scales from 0.7M to 7.4M, a 10x improvement. Atomic mode reaches 6.8M with minimal overhead.

RCU is the slowest for writes (~400K ops/sec) because every write allocates memory, copies 8KB of data, and defers a free. This is expected — RCU is designed for reads, not writes.

### Where RCU Shines

Switch to 80% reads with Zipfian distribution, and the picture changes:

| Threads | Global | Sharded | Per-CPU | RCU | Atomic |
|---------|--------|---------|---------|-----|--------|
| 1 | 3.2M | 2.5M | 2.4M | 1.3M | 2.9M |
| 8 | 3.7M | 3.9M | 5.4M | 2.7M | 5.3M |
| 16 | 3.6M | 5.1M | 6.1M | 2.5M | 5.8M |

RCU jumps from 400K (write-only) to 2.7M (read-heavy). The gap narrows because RCU reads are nearly free — no lock, no atomic operation, just a pointer dereference.

### CPU Utilization: The Hidden Cost

Throughput alone doesn't tell the full story. Look at CPU usage at 16 threads (hotspot, write-only):

| Mode | User CPU% | System CPU% | Idle% | Total CPU% |
|------|-----------|-------------|-------|------------|
| Global | 8.7% | 60.9% | 29.6% | 69.6% |
| Sharded | 5.2% | 30.9% | 62.9% | 36.1% |
| Per-CPU | 14.6% | 29.3% | 56.1% | 43.9% |
| RCU | 1.5% | 46.6% | 26.0% | 48.1% |
| Atomic | 3.3% | 23.3% | 73.3% | 26.7% |

Global mode burns 60.9% of CPU in kernel space — that's threads spinning on the lock, doing zero useful work. Atomic mode uses only 26.7% total CPU while delivering nearly double the throughput. That's the difference between wasting resources and using them.

The efficiency metric (throughput per CPU%) tells it clearly: atomic mode gets ~255K ops/sec per 1% CPU used. Global mode gets ~51K. Atomic is 5x more efficient with the same hardware.

### The Contention Heatmap

The kernel module tracks which CPU hits which shard, exported via debugfs. Under the hotspot workload with sharded locks, the heatmap shows:

- Shard 0: ~541,000 hits per CPU (the hot key maps here)
- All other shards: ~900 hits per CPU

That's a 600:1 imbalance. All 6 CPUs are fighting over shard 0's lock while the other 63 shards sit nearly idle. This is the "hot partition" problem that plagues distributed databases.

Under Zipfian distribution, the heatmap shows a smooth decay: shard 0 gets ~140K hits, shard 1 gets ~62K, shard 2 gets ~39K. This matches the theoretical Zipfian curve and demonstrates why uniform benchmarks are misleading — real workloads always have hot spots.

---

## What the Kernel Taught Me

### 1. The right lock depends on your read/write ratio

There's no universally "best" synchronization strategy. Global locks are fine for low-contention, single-threaded code. Sharding works when keys distribute evenly. Per-CPU is ideal for write-heavy counters. RCU dominates when reads vastly outnumber writes. Atomics win for simple increment/read operations.

### 2. System CPU% is the smell of lock contention

When your system CPU percentage is high but throughput is flat, threads are spinning on locks instead of doing work. In our global lock test, 60.9% system CPU produced only 3.6M ops/sec. Atomic mode used 23.3% system CPU for 6.8M ops/sec. Monitor `sys%` in production — it's your early warning.

### 3. Uniform benchmarks lie

Testing with uniform random keys makes sharding look great. But real workloads follow Zipfian distributions where a few keys get most of the traffic. Under hotspot conditions, sharding barely outperforms a global lock because all threads converge on the same shard. Always benchmark with realistic access patterns.

### 4. False sharing is a silent killer

Adding `____cacheline_aligned` to the shard struct ensures each lock sits on its own 64-byte cache line. Without this, two adjacent shard locks could share a cache line, and modifying one invalidates the cache for the other — even though they're independent locks. One annotation, measurable performance difference.

### 5. RCU is brilliant but not magic

RCU's read path is essentially free, but writes are expensive: memory allocation, full data copy, deferred free. Under write-heavy workloads, RCU was 10x slower than a simple spinlock. It's a specialized tool for read-dominated data structures, not a general-purpose replacement for locks.

---

## The Code

The full project is structured as:

```
klocklab/
├── common/klocklab.h      # Shared header (ioctl definitions, structs)
├── module/klocklab.c       # Kernel module (5 sync modes + debugfs)
├── module/Makefile
├── bench/bench.c           # Userspace benchmark (Zipfian, burst, R/W mix)
├── bench/Makefile
├── scripts/run_all.sh      # Automated benchmark suite
├── scripts/collect_perf.sh # perf lock profiling
└── analysis/plot_results.py # Visualization (throughput, latency, heatmap)
```

The kernel module registers as a misc device, supports three ioctls (update, get_stats, reset), and exposes live statistics through debugfs. The benchmark supports configurable thread counts, key distributions (uniform, hotspot, Zipfian), read/write ratios, burst patterns, and CPU pinning.

---

## Conclusion

Lock contention isn't an abstract computer science concept. It's a measurable, quantifiable performance problem that affects every multi-threaded system. The progression from global lock → sharded locks → per-CPU → RCU → lock-free atomics represents decades of real engineering work in the Linux kernel, databases, and distributed systems.

Building klocklab gave me concrete numbers to attach to these strategies. A global lock at 16 threads burns 61% CPU in kernel space for 3.6M ops/sec. Lock-free atomics use 23% CPU for 6.8M ops/sec. That's not a theoretical improvement — it's measured, in the kernel, under realistic workloads.

The next time your database slows down under load, check the system CPU percentage. If it's high and throughput is flat, you're looking at lock contention. The solution depends on your read/write ratio, your access patterns, and how much complexity you're willing to accept. But now you have the data to make that choice.
