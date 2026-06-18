#!/bin/bash
# BUG-2 (x86 send-bearing cross-method inline-J2J) repro driver.
#
# READ README.md FIRST. The `asString` corruption (modes `corrupt`/`clean`) is a
# SCOPING ARTIFACT, not the real bug. The REAL send-bearing defect is `real`.
#
# Usage:
#   ./run.sh real      # FAITHFUL real-bug repro: full config + SENDS=1 -> #asForm DNU (menu path)
#   ./run.sh artifact  # the SCOPING-ARTIFACT asString corruption (caller-J2J/callee-no-J2J mismatch)
#   ./run.sh noartifact # same scope but callees ALSO inline-J2J'd -> CLEAN (proves it's an artifact)
#   ./run.sh lldb      # launch the real repro under lldb (Rosetta x86)
#
# Requires: build-x86/test_load_image (x86 JIT build). Image at $IMG.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build-x86/test_load_image}"
IMG="${IMG:-/tmp/harness/Pharo.image}"

if [ ! -x "$BIN" ]; then echo "build x86 first: cmake --build build-x86 --target test_load_image"; exit 1; fi
if [ ! -f "$IMG" ]; then
  echo "image $IMG missing. Restore: cp /tmp/harness/Pharo-prepped.image $IMG"
  exit 1
fi

COMMON="PHARO_DET_SCHED=1 PHARO_MAX_STEPS=2000000000000 PHARO_X86_JIT=1"
CALLEES="asString,species,size,new:,replaceFrom:to:with:startingAt:"

case "${1:-real}" in
  real)
    echo "=== REAL bug: full config + SENDS=1 -> expect #asForm DNU (menu path), NO EVAL-RESULT ==="
    env PHARO_T1_X86_XMETHOD_SENDS=1 $COMMON \
      "$BIN" "$IMG" eval "3 + 4" 2>&1 \
      | grep -E "doesNotUnderstand: #(asForm|capitalized)|EVAL-RESULT=|DNU-STACK" | head -12
    ;;
  artifact)
    echo "=== SCOPING ARTIFACT: asString inline-J2J but callees NOT -> #capitalized DNU ==="
    env PHARO_T1_X86_J2J_SEL=asString PHARO_T1_X86_J2J_CLASS=Symbol $COMMON \
      "$BIN" "$IMG" eval "3 + 4" 2>&1 \
      | grep -oE "doesNotUnderstand: #(capitalized|asForm)|EVAL-RESULT=[0-9]+" | head -1
    ;;
  noartifact)
    echo "=== NO ARTIFACT: asString + ALL callees inline-J2J'd (SENDS on) -> CLEAN EVAL-RESULT=7 ==="
    env PHARO_T1_X86_J2J_SEL="$CALLEES" PHARO_T1_X86_XMETHOD_SENDS=1 $COMMON \
      "$BIN" "$IMG" eval "3 + 4" 2>&1 \
      | grep -oE "doesNotUnderstand: #(capitalized|asForm)|EVAL-RESULT=[0-9]+" | head -1
    ;;
  lldb)
    echo "=== lldb (Rosetta x86) on the REAL repro. See README for the plan. ==="
    env PHARO_T1_X86_XMETHOD_SENDS=1 PHARO_J2J_NEST_TRACE=1 $COMMON \
      lldb -- "$BIN" "$IMG" eval "3 + 4"
    ;;
  *) echo "usage: $0 {real|artifact|noartifact|lldb}"; exit 2;;
esac
