#!/bin/bash
# Per-class diff of a sweep against a recorded per-class-totals.txt baseline.
#   scripts/sweep-diff.sh <sweep-outdir> <baseline-per-class-totals.txt>
# Prints only classes whose P/F/E/T differ, plus classes present in one and not
# the other -- which is how a lost batch shows up.
D=${1:?usage: sweep-diff.sh <sweep-outdir> <baseline-totals>}
B=${2:?usage: sweep-diff.sh <sweep-outdir> <baseline-totals>}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tr '\r' '\n' < "$D/all_results.txt" 2>/dev/null | awk '
  /^=== / { cls=$2 }
  # "=== BATCH COMPLETE ===" is a marker, not a class
  /^Total:/ { if (cls != "BATCH") printf "%s %s\n", cls, $0 }' | sort -u > "$tmp/new"
[ -s "$D/retry_results.txt" ] && tr '\r' '\n' < "$D/retry_results.txt" | awk '
  /^=== / { cls=$2 }
  /^Total:/ { if (cls != "BATCH") printf "%s %s\n", cls, $0 }' | sort -u >> "$tmp/new"
awk '{ cls=$1; $1=""; sub(/^ +/,""); printf "%s %s\n", cls, $0 }' "$B" | sort -u > "$tmp/old"
awk '
  NR==FNR { key=$1; $1=""; sub(/^ +/,""); old[key]=$0; next }
  { key=$1; $1=""; sub(/^ +/,""); new[key]=$0 }
  END {
    printf "--- classes only in the NEW run\n  "
    for (k in new) if (!(k in old)) printf "%s ", k
    printf "\n--- classes only in the BASELINE (lost, or not reached yet)\n  "
    n=0; for (k in old) if (!(k in new)) n++
    printf "%d classes\n", n
    print "--- classes whose totals changed"
    for (k in new) if ((k in old) && new[k] != old[k])
        printf "  %-45s new: %s | was: %s\n", k, new[k], old[k]
  }' "$tmp/old" "$tmp/new" | sort
