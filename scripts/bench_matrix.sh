#!/bin/bash
# bench_matrix.sh — Full-matrix UADK scheduling benchmark
#
# Covers: SYNC/ASYNC × init2/old × RR/LOOP/HUNGRY/INSTR × HW/MIX × threads
# Outputs CSV to stdout and results/bench_matrix.csv on the remote server.
#
# Usage:
#   ./scripts/bench_matrix.sh              # run on local server
#   REMOTE=1 ./scripts/bench_matrix.sh     # run via SSH to $SERVER

set -euo pipefail

SERVER="${SERVER:-root@192.168.90.141}"
REMOTE_DIR="${REMOTE_DIR:-/root/hch/uadk_new}"
REMOTE="${REMOTE:-0}"
DURATION="${DURATION:-5}"
RESULTS_DIR="${RESULTS_DIR:-results}"
BENCH_BIN="${BENCH_BIN:-./bench_sched}"

# ── matrix dimensions ──────────────────────────────────────────────
MODES="sync async"
INITS="init2 old"
SCHEDS="rr loop hungry instr"
TASKS="hw mix"
THREADS="1 4 16 32"

# ── helpers ────────────────────────────────────────────────────────
now() { date '+%Y-%m-%d %H:%M:%S'; }

run_one() {
	local mode=$1 init=$2 sched=$3 task=$4 threads=$5

	# Skip invalid combinations
	if [ "$init" = "old" ] && [ "$task" = "mix" ]; then
		return 0
	fi
	# INSTR + SYNC may not be supported everywhere
	if [ "$sched" = "instr" ] && [ "$mode" = "sync" ]; then
		# Still try it, but note it might fail
		:
	fi

	local label="${mode}_${init}_${sched}_${task}_j${threads}"
	echo "=== [$(now)] $label ===" >&2

	local cmd="$BENCH_BIN --mode $mode --init $init --sched $sched --task $task --threads $threads --duration $DURATION"

	if [ "$REMOTE" = "1" ]; then
		ssh "$SERVER" "cd $REMOTE_DIR && $cmd" 2>&1 || echo "REMOTE_FAIL:$label" >&2
	else
		cd "$(dirname "$BENCH_BIN")" && $cmd 2>&1 || echo "LOCAL_FAIL:$label" >&2
	fi
}

# ── CSV header ─────────────────────────────────────────────────────
csv_file="${RESULTS_DIR}/bench_matrix.csv"
mkdir -p "$RESULTS_DIR"
echo "mode,init,sched,task,threads,duration,kops,avg_lat_us,p50_lat_us,p99_lat_us" > "$csv_file"

# ── parse output ───────────────────────────────────────────────────
parse_and_append() {
	local mode=$1 init=$2 sched=$3 task=$4 threads=$5
	local output=$6

	# Extract Kops line: "  Sent: NNN  Completed: NNN  Time: N.Ns  Kops: NNN.N"
	local kops=$(echo "$output" | grep -oP 'Kops:\s*\K[\d.]+' | head -1)
	[ -z "$kops" ] && kops="0"

	# Extract latency line: "  Latency: avg=N.Nus  P50=N.Nus  P99=N.Nus"
	local avg_lat=$(echo "$output" | grep -oP 'avg=\K[\d.]+' | head -1)
	local p50_lat=$(echo "$output" | grep -oP 'P50=\K[\d.]+' | head -1)
	local p99_lat=$(echo "$output" | grep -oP 'P99=\K[\d.]+' | head -1)
	[ -z "$avg_lat" ] && avg_lat="0"
	[ -z "$p50_lat" ] && p50_lat="0"
	[ -z "$p99_lat" ] && p99_lat="0"

	echo "${mode},${init},${sched},${task},${threads},${DURATION},${kops},${avg_lat},${p50_lat},${p99_lat}" >> "$csv_file"
}

# ── main loop ──────────────────────────────────────────────────────
total=$((2 * 2 * 4 * 2 * 4))  # 128 combinations
count=0

echo "=== UADK Benchmark Matrix ===" >&2
echo "Total combinations: $total (some invalid combos skipped)" >&2
echo "Output: $csv_file" >&2
echo "" >&2

for mode in $MODES; do
  for init in $INITS; do
    for sched in $SCHEDS; do
      for task in $TASKS; do
        for threads in $THREADS; do
          count=$((count + 1))

          # Skip invalid combinations
          if [ "$init" = "old" ] && [ "$task" = "mix" ]; then
            echo "[$count/$total] SKIP: $mode $init $sched $task j=$threads (old+mix unsupported)" >&2
            continue
          fi

          echo -n "[$count/$total] $mode $init $sched $task j=$threads ... " >&2

          output=$(run_one "$mode" "$init" "$sched" "$task" "$threads" 2>&1) || true
          parse_and_append "$mode" "$init" "$sched" "$task" "$threads" "$output"

          kops=$(echo "$output" | grep -oP 'Kops:\s*\K[\d.]+' | head -1 || echo "?")
          echo "Kops=$kops" >&2
        done
      done
    done
  done
done

echo "" >&2
echo "=== Done: $count combinations tested ===" >&2
echo "Results: $csv_file" >&2
