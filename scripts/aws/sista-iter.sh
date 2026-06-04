#!/usr/bin/env bash
# sista-iter.sh — one-command edit→build→test loop for the x86 Sista port.
#
# From the worktree after committing a change on jit-x86:
#   ./scripts/aws/sista-iter.sh            # push, remote incremental build, run with Sista
#   ./scripts/aws/sista-iter.sh nobuild    # skip build, just re-run the VM
#
# Pushes jit-x86, syncs it on the live spot box (state.env PUBLIC_IP),
# incremental-rebuilds, runs test_load_image with PHARO_SISTA_DISPATCH=1, and
# prints the [SISTA-x86] OK/bail stats + any crash signal. Result is also
# written to /tmp/sista-iter.log locally.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/config.env"
source "$HERE/load-creds.sh" >/dev/null 2>&1
source "$HERE/state.env"

PEM="${PEM:-$HOME/.ssh/${KEY_NAME}.pem}"
IP="${PUBLIC_IP:?no PUBLIC_IP in state.env — provision first}"
MODE="${1:-build}"

echo "== push jit-x86 =="
git -C "$HERE/../.." push origin jit-x86 2>&1 | tail -2

BUILD='cd ~/src/iospharo && git fetch -q origin jit-x86 && git reset -q --hard origin/jit-x86 && \
       cmake --build build -j"$(nproc)" 2>&1 | tail -5'
[ "$MODE" = nobuild ] && BUILD='echo "(skip build)"'

echo "== remote sync + build + Sista run =="
timeout 300 ssh -i "$PEM" -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new ubuntu@"$IP" "
  set -e
  $BUILD
  echo '== run: PHARO_SISTA_DISPATCH=1 =='
  PHARO_SISTA_DISPATCH=1 timeout 90 ./build/test_load_image ~/harness/Pharo.image > /tmp/sista_run.log 2>&1
  echo \"VM_EXIT=\$?\"
  echo '--- crash? ---'
  grep -iE 'segmentation|sigsegv|signal |abort|assert fail' /tmp/sista_run.log | head
  echo '--- [SISTA-x86] OK/bail (last) ---'
  grep -E '\[SISTA-x86\]' /tmp/sista_run.log | tail -3
  echo '--- JIT stats tail ---'
  tail -6 /tmp/sista_run.log
" 2>&1 | tee /tmp/sista-iter.log
