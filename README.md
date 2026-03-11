# klocklab

A Linux kernel module for benchmarking synchronization strategies under concurrent workloads. Implements five locking modes — global spinlock, sharded spinlocks, per-CPU counters, RCU (Read-Copy-Update), and lock-free atomics — and measures throughput, latency, CPU utilization, and contention heatmaps under realistic access patterns.

## Why

Lock contention is one of the most common performance bottlenecks in multi-core systems. Databases, operating systems, and networking stacks all face the same fundamental question: how do you let many threads access shared data without them stepping on each other?

This project reproduces that problem at the kernel level and benchmarks five solutions with real measurements.

## Project Structure

```
klocklab/
├── common/klocklab.h          # Shared header (ioctl definitions, mode enums, structs)
├── module/
│   ├── klocklab.c             # Kernel module (5 sync modes, debugfs, latency tracking)
│   └── Makefile
├── bench/
│   ├── bench.c                # Userspace benchmark (Zipfian, hotspot, burst, R/W mix)
│   └── Makefile
├── scripts/
│   ├── run_all.sh             # Automated benchmark suite across all modes and workloads
│   └── collect_perf.sh        # perf lock profiling helper
├── analysis/
│   └── plot_results.py        # Generates throughput, latency, CPU, and heatmap plots
├── article/
│   └── medium_article.md      # Writeup of findings
└── README.md
```

## Synchronization Modes

| Mode | Name | Description |
|------|------|-------------|
| 0 | Global | Single spinlock protecting all 1024 keys |
| 1 | Sharded | 64 spinlocks, each protecting ~16 keys. Cache-line aligned to prevent false sharing |
| 2 | Per-CPU | Each CPU gets a private copy of all counters. No locking needed |
| 3 | RCU | Read-Copy-Update. Near-zero-cost reads, copy-on-write for writes |
| 4 | Atomic | Lock-free `atomic64_t` counters. Hardware-level synchronization |

## Workload Distributions

| Distribution | Description |
|-------------|-------------|
| Uniform | Every key has equal probability |
| Hotspot | 90% of ops hit key 0, 10% spread randomly |
| Zipfian (α=1.2) | Power-law distribution — a few keys are very hot, most are cold. Matches real-world database/cache access patterns |

The benchmark also supports configurable read/write ratios and burst patterns.

## Prerequisites

- Linux machine (or VM) with root access
- Kernel headers: `linux-headers-$(uname -r)`
- Build tools: `build-essential`
- Python 3 with `matplotlib` and `numpy` (for plotting)
- Optional: `perf` tools (for lock profiling)

### Install on Ubuntu/Debian

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) python3-matplotlib python3-numpy
```

## Build

```bash
# Build the kernel module
cd module
make

# Build the benchmark
cd ../bench
make
```

## Run

### Quick test (single mode)

```bash
# Load module in sharded mode
sudo insmod module/klocklab.ko mode=1

# Run: 8 threads, 200K ops each, 1024 keys, Zipfian distribution, pinned to CPUs
./bench/bench -t 8 -o 200000 -k 1024 -d zipfian -z 1.2 -P

# View live stats from kernel
sudo cat /sys/kernel/debug/klocklab/stats

# View contention heatmap (CPU x shard)
sudo cat /sys/kernel/debug/klocklab/heatmap

# Unload
sudo rmmod klocklab
```

### Full benchmark suite

```bash
cd scripts
chmod +x run_all.sh
sudo ./run_all.sh
```

This tests all 5 modes across 5 workload patterns (hotspot, zipfian, uniform, read-heavy, burst) with 1, 2, 4, 8, and 16 threads. Results are saved to `results.csv` and heatmap CSVs are exported per mode.

You can customize with environment variables:

```bash
OPS_PER_THREAD=500000 KEY_SPACE=2048 PIN=1 sudo -E ./run_all.sh
```

### Generate plots

```bash
cd scripts
python3 ../analysis/plot_results.py
```

This generates:
- `throughput_vs_threads.png` — throughput comparison across modes and workloads
- `latency_vs_threads.png` — average latency per operation
- `cpu_utilization.png` — user vs system CPU breakdown
- `cpu_efficiency.png` — throughput per CPU% (resource efficiency)
- `contention_heatmap.png` — which CPUs hit which shards

### perf lock profiling

```bash
cd scripts
chmod +x collect_perf.sh
sudo ./collect_perf.sh 0 16 200000 1024 90 1
# Args: mode threads ops_per_thread key_space hotspot_pct pin
```

## Benchmark CLI Options

```
Usage: bench [options]
  -t <threads>         number of worker threads (default: 4)
  -o <ops_per_thread>  operations per thread (default: 200000)
  -k <key_space>       key range [0, key_space) (default: 1024)
  -d <distribution>    uniform|hotspot|zipfian (default: hotspot)
  -H <hotspot_pct>     hotspot percentage 0-100 (default: 90)
  -z <zipf_alpha>      Zipfian skew parameter (default: 1.2)
  -r <read_pct>        read percentage 0-100 (default: 0)
  -b <burst_size>      ops per burst, 0=no burst (default: 0)
  -p <burst_pause_us>  pause between bursts in microseconds (default: 100)
  -P                   pin threads round-robin to CPUs
  -h                   show help
```

## debugfs Interface

When the module is loaded, live stats are available at:

```bash
# Full statistics (mode, counters, latency histogram, shard hits)
sudo cat /sys/kernel/debug/klocklab/stats

# Contention heatmap (CSV: rows=CPUs, columns=shards)
sudo cat /sys/kernel/debug/klocklab/heatmap
```

## Running in VirtualBox

If you're on Windows or macOS, you can run this in a VirtualBox VM:

1. Create an Ubuntu VM (Server or Desktop)
2. Allocate 4+ CPUs (more CPUs = more interesting results) and 2 GB RAM
3. Install VirtualBox Guest Additions for shared folders:
   ```bash
   sudo apt install virtualbox-guest-utils virtualbox-guest-dkms
   sudo usermod -aG vboxsf $USER
   sudo reboot
   ```
4. Set up a shared folder in VirtualBox settings pointing to your project directory
5. Copy files from the shared folder to your home directory:
   ```bash
   cp -r /media/sf_klocklab ~/klocklab
   ```
6. Build and run as described above

## Sample Results

Under hotspot workload (16 threads, write-only):

| Mode | Throughput | System CPU% | Efficiency |
|------|-----------|-------------|------------|
| Global | 3.6M ops/s | 60.9% | Low — threads spinning on lock |
| Sharded | 4.3M ops/s | 30.9% | Medium — contention on hot shard |
| Per-CPU | 7.4M ops/s | 29.3% | High — no lock contention |
| RCU | 0.4M ops/s | 46.6% | Low for writes — designed for reads |
| Atomic | 6.8M ops/s | 23.3% | Highest — hardware synchronization |

## License

GPL-2.0 (required for Linux kernel modules)
