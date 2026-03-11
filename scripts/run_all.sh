#!/usr/bin/env bash
set -euo pipefail

THREADS_LIST=("1" "2" "4" "8" "16")
OPS_PER_THREAD="${OPS_PER_THREAD:-200000}"
KEY_SPACE="${KEY_SPACE:-1024}"
PIN="${PIN:-1}"
OUT_CSV="${OUT_CSV:-results.csv}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_DIR="$ROOT_DIR/module"
BENCH_DIR="$ROOT_DIR/bench"

if [[ ! -f "$MODULE_DIR/klocklab.ko" ]]; then
  echo "Kernel module not built. Run: (cd module && make)"
  exit 1
fi

if [[ ! -f "$BENCH_DIR/bench" ]]; then
  echo "Benchmark not built. Run: (cd bench && make)"
  exit 1
fi

HEADER="mode,threads,ops_per_thread,key_space,distribution,read_pct,throughput_ops_s"
HEADER="$HEADER,lat_min_ns,lat_max_ns,lat_avg_ns,total_updates,total_reads,total_sum"
HEADER="$HEADER,cpu_usr_pct,cpu_sys_pct,cpu_idle_pct,cpu_iowait_pct,cpu_total_pct"
echo "$HEADER" > "$OUT_CSV"

# Modes: 0=global 1=sharded 2=percpu 3=rcu 4=atomic
ALL_MODES=("0" "1" "2" "3" "4")

# Read aggregate CPU counters from /proc/stat
# Returns: user nice system idle iowait irq softirq steal
read_cpu_stat() {
  awk '/^cpu / {print $2, $3, $4, $5, $6, $7, $8, $9}' /proc/stat
}

# Compute CPU percentages between two snapshots
# Args: before_str after_str
# Output: usr_pct sys_pct idle_pct iowait_pct total_pct
compute_cpu_pct() {
  local before=($1)
  local after=($2)

  local d_user=$(( ${after[0]} - ${before[0]} ))
  local d_nice=$(( ${after[1]} - ${before[1]} ))
  local d_sys=$(( ${after[2]} - ${before[2]} ))
  local d_idle=$(( ${after[3]} - ${before[3]} ))
  local d_iowait=$(( ${after[4]} - ${before[4]} ))
  local d_irq=$(( ${after[5]} - ${before[5]} ))
  local d_softirq=$(( ${after[6]} - ${before[6]} ))
  local d_steal=$(( ${after[7]} - ${before[7]} ))

  local d_total=$(( d_user + d_nice + d_sys + d_idle + d_iowait + d_irq + d_softirq + d_steal ))

  if [[ "$d_total" -eq 0 ]]; then
    echo "0.0 0.0 100.0 0.0 0.0"
    return
  fi

  # Use awk for floating point
  echo "$d_user $d_nice $d_sys $d_idle $d_iowait $d_total" | awk '{
    usr = ($1 + $2) / $6 * 100
    sys = $3 / $6 * 100
    idle = $4 / $6 * 100
    iowait = $5 / $6 * 100
    total = usr + sys
    printf "%.1f %.1f %.1f %.1f %.1f", usr, sys, idle, iowait, total
  }'
}

run_bench() {
  local mode="$1"
  local dist="$2"
  local read_pct="$3"
  local extra_args="${4:-}"

  echo "Loading module mode=$mode"
  sudo insmod "$MODULE_DIR/klocklab.ko" mode="$mode"

  for t in "${THREADS_LIST[@]}"; do
    echo "  mode=$mode dist=$dist read_pct=$read_pct threads=$t"
    local CMD="$BENCH_DIR/bench -t $t -o $OPS_PER_THREAD -k $KEY_SPACE -d $dist -r $read_pct"
    if [[ "$PIN" == "1" ]]; then CMD="$CMD -P"; fi
    if [[ -n "$extra_args" ]]; then CMD="$CMD $extra_args"; fi

    # Snapshot CPU before
    local CPU_BEFORE
    CPU_BEFORE="$(read_cpu_stat)"

    OUTPUT=$($CMD)

    # Snapshot CPU after
    local CPU_AFTER
    CPU_AFTER="$(read_cpu_stat)"

    # Compute CPU usage
    local CPU_PCT
    CPU_PCT="$(compute_cpu_pct "$CPU_BEFORE" "$CPU_AFTER")"
    local CPU_USR CPU_SYS CPU_IDLE CPU_IOWAIT CPU_TOTAL
    read -r CPU_USR CPU_SYS CPU_IDLE CPU_IOWAIT CPU_TOTAL <<< "$CPU_PCT"

    THROUGHPUT=$(echo "$OUTPUT" | awk -F: '/throughput_ops\/s/ {gsub(/ /,"",$2); print $2}')
    LAT_MIN=$(echo "$OUTPUT" | awk -F: '/^lat_min_ns/ {gsub(/ /,"",$2); print $2}')
    LAT_MAX=$(echo "$OUTPUT" | awk -F: '/^lat_max_ns/ {gsub(/ /,"",$2); print $2}')
    LAT_AVG=$(echo "$OUTPUT" | awk -F: '/^lat_avg_ns/ {gsub(/ /,"",$2); print $2}')
    TOTAL_W=$(echo "$OUTPUT" | awk -F: '/total_writes/ {gsub(/ /,"",$2); print $2}')
    TOTAL_R=$(echo "$OUTPUT" | awk -F: '/total_reads/ {gsub(/ /,"",$2); print $2}')
    TOTAL_S=$(echo "$OUTPUT" | awk -F: '/total_sum/ {gsub(/ /,"",$2); print $2}')

    echo "    CPU: usr=${CPU_USR}% sys=${CPU_SYS}% idle=${CPU_IDLE}% total=${CPU_TOTAL}%"

    echo "$mode,$t,$OPS_PER_THREAD,$KEY_SPACE,$dist,$read_pct,$THROUGHPUT,$LAT_MIN,$LAT_MAX,$LAT_AVG,$TOTAL_W,$TOTAL_R,$TOTAL_S,$CPU_USR,$CPU_SYS,$CPU_IDLE,$CPU_IOWAIT,$CPU_TOTAL" >> "$OUT_CSV"
  done

  # Dump debugfs stats and heatmap for last run
  if [[ -f /sys/kernel/debug/klocklab/stats ]]; then
    echo "  --- debugfs stats (mode=$mode, last run) ---"
    sudo cat /sys/kernel/debug/klocklab/stats
  fi
  if [[ -f /sys/kernel/debug/klocklab/heatmap ]]; then
    echo "  --- heatmap (mode=$mode) ---"
    sudo cat /sys/kernel/debug/klocklab/heatmap > "heatmap_mode${mode}_${dist}_r${read_pct}.csv"
    echo "  Saved heatmap_mode${mode}_${dist}_r${read_pct}.csv"
  fi
  echo ""

  sudo rmmod klocklab
}

echo "=== Workload 1: Hotspot 90% (write-only) ==="
for m in "${ALL_MODES[@]}"; do run_bench "$m" "hotspot" 0 "-H 90"; done

echo "=== Workload 2: Zipfian (write-only) ==="
for m in "${ALL_MODES[@]}"; do run_bench "$m" "zipfian" 0 "-z 1.2"; done

echo "=== Workload 3: Uniform (write-only) ==="
for m in "${ALL_MODES[@]}"; do run_bench "$m" "uniform" 0; done

echo "=== Workload 4: Zipfian 80% read / 20% write ==="
for m in "${ALL_MODES[@]}"; do run_bench "$m" "zipfian" 80 "-z 1.2"; done

echo "=== Workload 5: Uniform with burst pattern ==="
for m in "${ALL_MODES[@]}"; do run_bench "$m" "uniform" 0 "-b 1000 -p 50"; done

echo "Saved results to $OUT_CSV"
