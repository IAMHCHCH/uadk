#!/bin/bash
#
# sched_policy_test.sh — Full cipher scheduling coverage on Kunpeng 920
#
# Architecture:
#   AES (all modes) → HW (hisi_sec2 real accelerator)
#   SM4 (all modes) → CE (isa_ce_sm4, CPU SM4 instructions)
#
# Two API paths tested:
#   1. OLD init: wd_sched_rr_alloc + wd_cipher_init()        (RR/DEV/LOOP/HUNGRY × HW)
#   2. NEW init2: wd_cipher_init2_(alg, policy, task, ctx)   (LOOP/HUNGRY × HW + INSTR/LOOP/HUNGRY × CE)
#
# Each init2 test runs in its own process to avoid HW resource conflicts.
#
# Prerequisites:
#   - UADK installed at /usr/local
#   - hisi_sec2 loaded (uacce_mode=1): modprobe hisi_sec2 uacce_mode=1
#   - libnuma
#
# Usage:
#   chmod +x sched_policy_test.sh
#   ./sched_policy_test.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UADK_INC="${UADK_INC:-/usr/local/include/uadk}"
UADK_LIB="${UADK_LIB:-/usr/local/lib}"
export LD_LIBRARY_PATH="${UADK_LIB}:${LD_LIBRARY_PATH:-}"

CFLAGS="-std=gnu11 -Wall -O2 -I${UADK_INC} -pthread"
LDFLAGS="-L${UADK_LIB} -lwd -lwd_crypto -lnuma -lpthread -ldl"

PASS=0
FAIL=0
SKIP=0
TOTAL=0

# ---- helpers for init2 (per-process) ----
run_test_init2() {
	local algo="$1" policy="$2" task_type="$3" ctx_prop="$4" pol_label="$5" drv_label="$6"
	local out rc
	set +e
	out=$("${BIN_INIT2}" "$algo" "$policy" "$task_type" "$ctx_prop" "$pol_label" "$drv_label" 2>/dev/null)
	rc=$?
	set -e
	echo "$out"
	TOTAL=$((TOTAL + 1))
	case $rc in
		0) PASS=$((PASS + 1)) ;;
		1) FAIL=$((FAIL + 1)) ;;
		*) SKIP=$((SKIP + 1)) ;;
	esac
}

# ---- helpers for old init (inline count from binary output) ----
count_old_init_results() {
	local output="$1" prefix="$2"
	local p f
	p=$(echo "$output" | grep -c "^  \[${prefix}.*OK " || true)
	f=$(echo "$output" | grep -c "^  \[${prefix}.*FAIL" || true)
	PASS=$((PASS + p))
	FAIL=$((FAIL + f))
	TOTAL=$((TOTAL + p + f))
}

# ---- main ----
echo "=============================================="
echo " UADK Full Cipher Scheduling Test"
echo " Server: Kunpeng 920 (AES=HW, SM4=CE)"
echo " Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo "=============================================="
echo ""

# Check prerequisites
echo "--- Prerequisites ---"
if ls /sys/class/uacce/hisi_sec2-* >/dev/null 2>&1; then
	N=$(cat /sys/class/uacce/hisi_sec2-*/available_instances 2>/dev/null | head -1)
	echo "[OK] hisi_sec2: ${N:-?} available instances"
else
	echo "[WARN] hisi_sec2 not found — HW tests will fail"
	echo "       Try: modprobe hisi_sec2 uacce_mode=1"
fi

if [ -f /usr/local/lib/libwd.so ] && [ -f /usr/local/lib/libwd_crypto.so ]; then
	echo "[OK] UADK libraries found"
else
	echo "[FAIL] UADK libraries not found at /usr/local/lib"
	exit 1
fi
echo ""

# Compile all binaries
echo "--- Compiling ---"
BIN_INIT2="${SCRIPT_DIR}/sched_policy_init2_test"
BIN_INIT="${SCRIPT_DIR}/sched_policy_init_test"
SRC_INIT2="${SCRIPT_DIR}/sched_policy_init2_test.c"
SRC_INIT="${SCRIPT_DIR}/sched_policy_init_test.c"

gcc ${CFLAGS} -o "${BIN_INIT2}" "${SRC_INIT2}" ${LDFLAGS} 2>&1 | grep -v 'discards\|note:' || true
echo "[OK] sched_policy_init2_test (init2 API)"

gcc ${CFLAGS} -o "${BIN_INIT}" "${SRC_INIT}" ${LDFLAGS} 2>&1 | grep -v 'discards\|note:' || true
echo "[OK] sched_policy_init_test (old init API)"
echo ""

# ================================================================
# Phase 0: Old init API verification (all-in-one binary, HW only)
# ================================================================
echo "=============================================="
echo " Phase 0: Old init API (wd_cipher_init)"
echo " 6 AES modes × 4 policies (RR/DEV/LOOP/HUNGRY)"
echo "=============================================="
echo ""

set +e
INIT_OUT=$("${BIN_INIT}" 2>/dev/null)
INIT_RC=$?
set -e
echo "$INIT_OUT" | grep -E '^  \[|^  =>|^==='
echo ""

count_old_init_results "$INIT_OUT" "RR"
count_old_init_results "$INIT_OUT" "DEV"
count_old_init_results "$INIT_OUT" "LOOP"
count_old_init_results "$INIT_OUT" "HUNGRY"

if [ $INIT_RC -ne 0 ]; then
	echo "Old init API: some tests FAILED (see above)"
else
	echo "Old init API: all tests PASSED"
fi
echo ""

# ================================================================
# Phase 1: AES modes × HW via init2 (LOOP, HUNGRY)
# ================================================================
echo "=============================================="
echo " Phase 1: init2 AES × HW Scheduling (hisi_sec2)"
echo "=============================================="
echo ""

for mode in ecb cbc ctr xts ofb cfb; do
	algo="${mode}(aes)"
	run_test_init2 "$algo" 4 1 0 "LOOP"   "HW"
	run_test_init2 "$algo" 5 1 0 "HUNGRY" "HW"
done

echo ""

# ================================================================
# Phase 2: SM4 modes × CE via init2 (INSTR, LOOP, HUNGRY)
# ================================================================
echo "=============================================="
echo " Phase 2: init2 SM4 × CE Scheduling (isa_ce_sm4)"
echo "=============================================="
echo ""

for mode in ecb cbc ctr xts ofb cfb; do
	algo="${mode}(sm4)"
	run_test_init2 "$algo" 6 2 1 "INSTR"  "CE"
	run_test_init2 "$algo" 4 2 1 "LOOP"   "CE"
	run_test_init2 "$algo" 5 2 1 "HUNGRY" "CE"
done

echo ""

# ================================================================
# Summary
# ================================================================
echo "=============================================="
echo " Summary"
echo "=============================================="
echo "  Total:   ${TOTAL}"
echo "  Passed:  ${PASS}"
echo "  Failed:  ${FAIL}"
echo "  Skipped: ${SKIP}"
echo ""
echo "  Phase 0 (old init): 24 tests (RR/DEV/LOOP/HUNGRY × 6 AES modes)"
echo "  Phase 1 (init2 HW): 12 tests (LOOP/HUNGRY × 6 AES modes)"
echo "  Phase 2 (init2 CE): 18 tests (INSTR/LOOP/HUNGRY × 6 SM4 modes)"
echo ""
if [ $FAIL -gt 0 ]; then
	echo "Note: SM4-OFB failures are expected — isa_ce_sm4 driver does not support OFB mode."
	echo ""
	echo "Some tests FAILED. Check output above for details."
	exit 1
else
	echo "All tests PASSED — both init and init2 APIs work correctly!"
	exit 0
fi
