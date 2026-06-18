#!/bin/bash
# BUG-2 (x86 send-bearing cross-method inline-J2J corruption) repro driver.
# Usage:
#   ./run.sh corrupt   # deterministic corruption (asString swaps a value to a class)
#   ./run.sh clean      # the clean control (same shape, isolated -> CORRECT)
#   ./run.sh lldb       # launch the corrupt repro under lldb (Rosetta x86)
#
# Requires: build-x86/test_load_image (the x86 JIT build; default flag=1
# sp-residency). A prepped Pharo image at $IMG (override with IMG=...).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build-x86/test_load_image}"
IMG="${IMG:-/tmp/harness/Pharo.image}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ ! -x "$BIN" ]; then echo "build x86 first: cmake --build build-x86 --target test_load_image"; exit 1; fi
if [ ! -f "$IMG" ]; then
  echo "image $IMG missing. Restore: cp /tmp/harness/Pharo-prepped.image $IMG  (or re-stage per CLAUDE.md 'Running Pharo test suites')."
  exit 1
fi

COMMON="PHARO_DET_SCHED=1 PHARO_MAX_STEPS=2000000000000 PHARO_X86_JIT=1"

case "${1:-corrupt}" in
  corrupt)
    echo "=== CORRUPT: expect '#capitalized'/'#asForm' DNU on a *class*, NO EVAL-RESULT ==="
    env PHARO_T1_X86_J2J_SEL=asString PHARO_T1_X86_XMETHOD_SENDS=1 $COMMON \
      "$BIN" "$IMG" eval "$(cat "$HERE/asString-corrupt.st")" 2>&1 \
      | grep -E "doesNotUnderstand: #|DNU-RCVR #cap|EVAL-RESULT" | head -6
    ;;
  clean)
    echo "=== CLEAN CONTROL: expect EVAL-RESULT='xfib10=143' (mechanism is sound) ==="
    env PHARO_T1_X86_J2J_SEL='x*' PHARO_T1_X86_XMETHOD_SENDS=1 $COMMON \
      "$BIN" "$IMG" eval "$(cat "$HERE/clean-control.st")" 2>&1 \
      | grep -oE "EVAL-RESULT='[^']*'|doesNotUnderstand: #[a-zA-Z:]+" | head -2
    ;;
  lldb)
    echo "=== lldb (Rosetta x86). See README for the breakpoint plan. ==="
    env PHARO_T1_X86_J2J_SEL=asString PHARO_T1_X86_XMETHOD_SENDS=1 PHARO_J2J_NEST_TRACE=1 $COMMON \
      lldb -- "$BIN" "$IMG" eval "$(cat "$HERE/asString-corrupt.st")"
    ;;
  *) echo "usage: $0 {corrupt|clean|lldb}"; exit 2;;
esac
