import csv
import os
import glob
from collections import defaultdict
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

MODE_NAMES = {
    "0": "global", "1": "sharded", "2": "percpu", "3": "rcu", "4": "atomic"
}
MODE_COLORS = {
    "0": "#1f77b4", "1": "#ff7f0e", "2": "#2ca02c", "3": "#d62728", "4": "#9467bd"
}

def load_results(path):
    rows = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["threads"] = int(row["threads"])
            row["throughput_ops_s"] = float(row["throughput_ops_s"])
            row["lat_avg_ns"] = float(row.get("lat_avg_ns", 0))
            row["cpu_usr_pct"] = float(row.get("cpu_usr_pct", 0))
            row["cpu_sys_pct"] = float(row.get("cpu_sys_pct", 0))
            row["cpu_idle_pct"] = float(row.get("cpu_idle_pct", 100))
            row["cpu_total_pct"] = float(row.get("cpu_total_pct", 0))
            rows.append(row)
    return rows

def group_by_workload(rows):
    workloads = defaultdict(list)
    for row in rows:
        key = (row.get("distribution", "hotspot"), row.get("read_pct", "0"))
        workloads[key].append(row)
    return workloads

def plot_throughput_by_workload(rows):
    workloads = group_by_workload(rows)
    n = len(workloads)
    fig, axes = plt.subplots(1, n, figsize=(5.5 * n, 5), squeeze=False)

    for idx, ((dist, rpct), wrows) in enumerate(sorted(workloads.items())):
        ax = axes[0][idx]
        grouped = defaultdict(list)
        for r in wrows:
            grouped[r["mode"]].append((r["threads"], r["throughput_ops_s"]))

        for mode, points in sorted(grouped.items()):
            points.sort()
            xs = [p[0] for p in points]
            ys = [p[1] for p in points]
            ax.plot(xs, ys, marker="o",
                    color=MODE_COLORS.get(mode, "gray"),
                    label=MODE_NAMES.get(mode, mode))

        ax.set_xlabel("Threads")
        ax.set_ylabel("Throughput (ops/sec)")
        ax.set_title(f"{dist} (read={rpct}%)")
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    fig.suptitle("Kernel Lock Contention Benchmark", fontsize=14)
    plt.tight_layout()
    plt.savefig("throughput_vs_threads.png", dpi=150)
    print("Saved throughput_vs_threads.png")
    plt.close()

def plot_latency_by_workload(rows):
    workloads = group_by_workload(rows)
    n = len(workloads)
    fig, axes = plt.subplots(1, n, figsize=(5.5 * n, 5), squeeze=False)

    for idx, ((dist, rpct), wrows) in enumerate(sorted(workloads.items())):
        ax = axes[0][idx]
        grouped = defaultdict(list)
        for r in wrows:
            grouped[r["mode"]].append((r["threads"], r["lat_avg_ns"]))

        for mode, points in sorted(grouped.items()):
            points.sort()
            xs = [p[0] for p in points]
            ys = [p[1] for p in points]
            ax.plot(xs, ys, marker="s",
                    color=MODE_COLORS.get(mode, "gray"),
                    label=MODE_NAMES.get(mode, mode))

        ax.set_xlabel("Threads")
        ax.set_ylabel("Avg Latency (ns)")
        ax.set_title(f"{dist} (read={rpct}%)")
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    fig.suptitle("Per-Operation Latency", fontsize=14)
    plt.tight_layout()
    plt.savefig("latency_vs_threads.png", dpi=150)
    print("Saved latency_vs_threads.png")
    plt.close()

def plot_cpu_utilization(rows):
    """Plot CPU usage (user vs system vs idle) per mode and thread count."""
    workloads = group_by_workload(rows)
    n = len(workloads)
    fig, axes = plt.subplots(2, n, figsize=(5.5 * n, 9), squeeze=False)

    for idx, ((dist, rpct), wrows) in enumerate(sorted(workloads.items())):
        # Top row: total CPU% (user + system)
        ax_top = axes[0][idx]
        grouped = defaultdict(list)
        for r in wrows:
            grouped[r["mode"]].append((r["threads"], r["cpu_total_pct"]))

        for mode, points in sorted(grouped.items()):
            points.sort()
            xs = [p[0] for p in points]
            ys = [p[1] for p in points]
            ax_top.plot(xs, ys, marker="o",
                        color=MODE_COLORS.get(mode, "gray"),
                        label=MODE_NAMES.get(mode, mode))

        ax_top.set_xlabel("Threads")
        ax_top.set_ylabel("CPU Usage (%)")
        ax_top.set_title(f"{dist} (read={rpct}%) - Total CPU")
        ax_top.legend(fontsize=7)
        ax_top.grid(True, alpha=0.3)
        ax_top.set_ylim(bottom=0)

        # Bottom row: stacked user vs system for each mode
        ax_bot = axes[1][idx]
        grouped_usr = defaultdict(list)
        grouped_sys = defaultdict(list)
        for r in wrows:
            grouped_usr[r["mode"]].append((r["threads"], r["cpu_usr_pct"]))
            grouped_sys[r["mode"]].append((r["threads"], r["cpu_sys_pct"]))

        for mode in sorted(grouped_usr.keys()):
            pts_u = sorted(grouped_usr[mode])
            pts_s = sorted(grouped_sys[mode])
            xs = [p[0] for p in pts_u]
            ys_u = [p[1] for p in pts_u]
            ys_s = [p[1] for p in pts_s]
            mname = MODE_NAMES.get(mode, mode)
            ax_bot.plot(xs, ys_s, marker="^", linestyle="--",
                        color=MODE_COLORS.get(mode, "gray"),
                        label=f"{mname} sys", alpha=0.7)
            ax_bot.plot(xs, ys_u, marker="o",
                        color=MODE_COLORS.get(mode, "gray"),
                        label=f"{mname} usr")

        ax_bot.set_xlabel("Threads")
        ax_bot.set_ylabel("CPU %")
        ax_bot.set_title(f"{dist} (read={rpct}%) - User vs System")
        ax_bot.legend(fontsize=6, ncol=2)
        ax_bot.grid(True, alpha=0.3)
        ax_bot.set_ylim(bottom=0)

    fig.suptitle("CPU Utilization by Mode", fontsize=14)
    plt.tight_layout()
    plt.savefig("cpu_utilization.png", dpi=150)
    print("Saved cpu_utilization.png")
    plt.close()


def plot_efficiency(rows):
    """Plot throughput per CPU% — shows how efficiently CPU time is used."""
    workloads = group_by_workload(rows)
    n = len(workloads)
    fig, axes = plt.subplots(1, n, figsize=(5.5 * n, 5), squeeze=False)

    for idx, ((dist, rpct), wrows) in enumerate(sorted(workloads.items())):
        ax = axes[0][idx]
        grouped = defaultdict(list)
        for r in wrows:
            cpu_pct = r["cpu_total_pct"]
            tput = r["throughput_ops_s"]
            # Efficiency: ops/sec per 1% CPU used
            eff = tput / cpu_pct if cpu_pct > 0 else 0
            grouped[r["mode"]].append((r["threads"], eff))

        for mode, points in sorted(grouped.items()):
            points.sort()
            xs = [p[0] for p in points]
            ys = [p[1] for p in points]
            ax.plot(xs, ys, marker="D",
                    color=MODE_COLORS.get(mode, "gray"),
                    label=MODE_NAMES.get(mode, mode))

        ax.set_xlabel("Threads")
        ax.set_ylabel("Throughput / CPU% (ops/sec/%)")
        ax.set_title(f"{dist} (read={rpct}%)")
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    fig.suptitle("CPU Efficiency (Throughput per CPU%)", fontsize=14)
    plt.tight_layout()
    plt.savefig("cpu_efficiency.png", dpi=150)
    print("Saved cpu_efficiency.png")
    plt.close()


def plot_heatmaps():
    """Plot contention heatmaps from CSV files exported by debugfs."""
    heatmap_files = sorted(glob.glob("heatmap_mode*.csv"))
    if not heatmap_files:
        print("No heatmap CSV files found, skipping heatmap plots.")
        return

    n = len(heatmap_files)
    fig, axes = plt.subplots(1, n, figsize=(max(6, 3 * n), 4), squeeze=False)

    for idx, path in enumerate(heatmap_files):
        ax = axes[0][idx]
        # Parse filename: heatmap_mode1_zipfian_r0.csv
        basename = os.path.splitext(os.path.basename(path))[0]
        parts = basename.split("_")
        mode_id = parts[0].replace("heatmap", "").replace("mode", "")
        label = MODE_NAMES.get(mode_id, mode_id)
        if len(parts) > 1:
            label += f" ({parts[1]}"
            if len(parts) > 2:
                label += f" {parts[2]}"
            label += ")"

        # Read CSV: first column is cpu, rest are shard columns
        cpus = []
        data = []
        with open(path, newline="") as f:
            reader = csv.reader(f)
            next(reader)  # skip header
            for row in reader:
                cpus.append(int(row[0]))
                data.append([int(x) for x in row[1:]])

        if not data:
            continue

        arr = np.array([[float(x) for x in row] for row in data])
        # Only show shards that have any activity
        active_shards = arr.sum(axis=0) > 0
        if active_shards.any():
            arr_active = arr[:, active_shards]
            shard_labels = [str(i) for i, a in enumerate(active_shards) if a]
        else:
            arr_active = arr
            shard_labels = [str(i) for i in range(arr.shape[1])]

        # Limit to 16 shards for readability
        if arr_active.shape[1] > 16:
            arr_active = arr_active[:, :16]
            shard_labels = shard_labels[:16]

        im = ax.imshow(arr_active, aspect="auto", cmap="YlOrRd")
        ax.set_xlabel("Shard")
        ax.set_ylabel("CPU")
        ax.set_title(label, fontsize=9)
        ax.set_yticks(range(len(cpus)))
        ax.set_yticklabels(cpus, fontsize=7)
        if len(shard_labels) <= 16:
            ax.set_xticks(range(len(shard_labels)))
            ax.set_xticklabels(shard_labels, fontsize=7)
        fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)

    fig.suptitle("Contention Heatmap (CPU x Shard)", fontsize=13)
    plt.tight_layout()
    plt.savefig("contention_heatmap.png", dpi=150)
    print("Saved contention_heatmap.png")
    plt.close()


if __name__ == "__main__":
    rows = load_results("results.csv")
    plot_throughput_by_workload(rows)
    plot_latency_by_workload(rows)
    plot_cpu_utilization(rows)
    plot_efficiency(rows)
    plot_heatmaps()
