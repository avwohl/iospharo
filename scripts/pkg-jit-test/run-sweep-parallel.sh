#!/usr/bin/env bash
# run-sweep-parallel.sh — run the 200-package sweep in N parallel workers, then
# merge their summaries.  Each worker drives a DISJOINT slice of the manifest
# through run-manifest.sh with its OWN output dir, and passes per-package prefixes
# via the PKG_PREFIXES env (so workers never race on the shared /tmp file).
#
# On a 16-vCPU box this turns a ~3h (load-dominated) sequential sweep into ~45m.
#
#   PHARO=/home/ubuntu/h3/pharo BASE_IMAGE=/home/ubuntu/h3/Pharo.image \
#   CUSTOM_VM=$PWD/build/test_load_image PHARO_X86_JIT=1 OUT=/home/ubuntu/pkg200 \
#   WORKERS=8 scripts/pkg-jit-test/run-sweep-parallel.sh [manifest.tsv]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
MANIFEST="${1:-$HERE/packages-200.tsv}"
N="${WORKERS:-8}"
BASE_OUT="${OUT:-/tmp/pkg200}"
mkdir -p "$BASE_OUT"
total=$(($(wc -l < "$MANIFEST") - 1))
# Round-robin split: the manifest is sorted big->small, so contiguous slices
# would pile every heavy package onto worker 0.  Interleaving (row i -> worker
# i%N) gives each worker a balanced mix of big and small.
hdr="$(head -1 "$MANIFEST")"
for w in $(seq 0 $((N - 1))); do printf '%s\n' "$hdr" > "$BASE_OUT/manifest.w$w.tsv"; done
tail -n +2 "$MANIFEST" | awk -v n="$N" -v base="$BASE_OUT/manifest.w" \
    '{ print >> (base ((NR-1)%n) ".tsv") }'
echo "$(date -u +%H:%M:%S) split $total packages round-robin across $N workers"

pids=()
for w in $(seq 0 $((N - 1))); do
    wm="$BASE_OUT/manifest.w$w.tsv"
    [ "$(($(wc -l < "$wm") - 1))" -ge 1 ] || break
    out="$BASE_OUT/w$w"; mkdir -p "$out"
    OUT="$out" nohup bash "$HERE/run-manifest.sh" "$wm" 1 100000 \
        > "$out/worker.log" 2>&1 &
    pids+=($!)
    echo "  worker $w: $(($(wc -l < "$wm") - 1)) packages -> $out (pid $!)"
done

echo "$(date -u +%H:%M:%S) ${#pids[@]} workers running; waiting..."
wait "${pids[@]}" 2>/dev/null

# Merge worker summaries into one.
printf 'label\tload\tcog_RESULT\tjit_RESULT\tjit_only_fails\n' > "$BASE_OUT/summary.tsv"
for w in $(seq 0 $((N - 1))); do
    tail -n +2 "$BASE_OUT/w$w/summary.tsv" 2>/dev/null
done >> "$BASE_OUT/summary.tsv"

done_n=$(($(wc -l < "$BASE_OUT/summary.tsv") - 1))
echo "$(date -u +%H:%M:%S) === SWEEP COMPLETE: $done_n packages ==="
awk -F'\t' 'NR>1{n++; if($2=="ok")ok++; if($5+0>0)reg++}
  END{printf "loaded: %d | load-failed: %d | with JIT-only failures: %d\n", ok, n-ok, reg}' "$BASE_OUT/summary.tsv"
echo "JIT-only failures (candidate x86-JIT regressions):"
awk -F'\t' 'NR>1 && $5+0>0{print "  "$1": "$5" ("$4")"}' "$BASE_OUT/summary.tsv"
