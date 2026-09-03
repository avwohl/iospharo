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
echo "--- classes only in the NEW run"
join -v1 -o 1.1 <(cut -d' ' -f1 "$tmp/new" | sort -u) <(cut -d' ' -f1 "$tmp/old" | sort -u) | tr '\n' ' '; echo
echo "--- classes only in the BASELINE (lost or renamed)"
join -v2 -o 2.1 <(cut -d' ' -f1 "$tmp/new" | sort -u) <(cut -d' ' -f1 "$tmp/old" | sort -u) | tr '\n' ' '; echo
echo "--- classes whose totals changed"
join <(sort "$tmp/new") <(sort "$tmp/old") -j1 -o 0,1.2,1.3,1.4,1.5,1.6,1.7,2.2,2.3,2.4,2.5,2.6,2.7 2>/dev/null \
  | awk '{ newv=$3" "$4" "$5" "$6" "$7; oldv=$9" "$10" "$11" "$12" "$13;
           if (newv != oldv) printf "  %-45s new: %s | was: %s\n", $1, newv, oldv }'
