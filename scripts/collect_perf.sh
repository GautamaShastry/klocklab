#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-0}"
THREADS="${2:-16}"
OPS_PER_THREAD="${3:-200000}"
KEY_SPACE="${4:-1024}"
HOTSPOT_PCT="${5:-90}"
PIN="${6:-1}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_DIR="$ROOT_DIR/module"
BENCH_DIR="$ROOT_DIR/bench"

if lsmod | grep -q '^klocklab'; then
  sudo rmmod klocklab
fi

sudo insmod "$MODULE_DIR/klocklab.ko" mode="$MODE"

echo "Recording perf lock data..."
sudo perf lock record "$BENCH_DIR/bench" "$THREADS" "$OPS_PER_THREAD" "$KEY_SPACE" "$HOTSPOT_PCT" "$PIN"

echo
echo "====== perf lock report ======"
sudo perf lock report

sudo rmmod klocklab