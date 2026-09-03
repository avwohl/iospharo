#!/usr/bin/env bash
# Three-way per-class comparison: arm64, x86 under emulation, x86 native.
#   scripts/sweep-3way.sh <arm-totals> <x86-emulated-totals> <x86-native-totals>
# Each argument is a per-class-totals.txt (class name, then the Total: line).
#
# The point is to separate what the EMULATOR invents from what x86 actually
# does.  A class that differs on emulated-x86 but agrees between arm64 and
# native-x86 is a Rosetta artifact; a class that differs on BOTH x86 runs is a
# real arch difference.
set -u
A=${1:?arm totals}; E=${2:?emulated-x86 totals}; N=${3:?native-x86 totals}
norm() { awk '{k=$1; $1=""; sub(/^ +/,""); print k"\t"$0}' "$1" | sort -u; }
join -t$'\t' <(norm "$A") <(norm "$E") 2>/dev/null | sort > /tmp/_3way_ae
join -t$'\t' /tmp/_3way_ae <(norm "$N") 2>/dev/null | sort > /tmp/_3way_all
echo "--- classes in all three runs: $(wc -l < /tmp/_3way_all | tr -d ' ')"
awk -F'\t' '
  { arm=$2; emu=$3; nat=$4
    if (arm==emu && emu==nat) { same++; next }
    if (arm==nat && emu!=nat) { rosetta++; ros[$1]=emu"  (both real: "arm")"; next }
    if (arm!=nat)             { real++;  rl[$1]="arm: "arm"  |  native x86: "nat"  |  emulated: "emu; next }
    other++; ot[$1]=arm" / "emu" / "nat }
  END {
    printf "  identical everywhere      %d\n", same+0
    printf "  EMULATOR ARTIFACT         %d  (arm == native x86, emulated differs)\n", rosetta+0
    printf "  real arch difference      %d  (arm != native x86)\n", real+0
    printf "  other                     %d\n", other+0
    if (rosetta) { print "\n--- emulator artifacts"; for (k in ros) printf "  %-44s %s\n", k, ros[k] }
    if (real)    { print "\n--- real arch differences"; for (k in rl) printf "  %-44s %s\n", k, rl[k] }
    if (other)   { print "\n--- other"; for (k in ot) printf "  %-44s %s\n", k, ot[k] }
  }' /tmp/_3way_all
