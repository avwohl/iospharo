#!/usr/bin/env bash
# confirm-jit.sh — isolate REAL x86-JIT codegen bugs from VM-core/env divergences.
#
# For each candidate package (the sweep's "JIT-only failures" vs Cog), reload it
# and run the custom VM with the x86 JIT ON and OFF, then diff the FAIL/ERR sets.
# A failure present with JIT ON but ABSENT with JIT OFF is JIT-caused (a real x86
# JIT bug); one that fails BOTH ways is VM-core/env (e.g. SSL stub, FFI), not JIT.
#
# stdin: one candidate per line, TAB-separated: label <TAB> load_expr <TAB> prefix
#   PHARO=.. BASE_IMAGE=.. CUSTOM_VM=.. OUT=.. confirm-jit.sh < candidates.tsv
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
RUNNER="$HERE/run_pkg_tests.st"
PHARO="${PHARO:-/home/ubuntu/h3/pharo}"
BASE_IMAGE="${BASE_IMAGE:-/home/ubuntu/h3/Pharo.image}"
CUSTOM_VM="${CUSTOM_VM:-/home/ubuntu/src/iospharo/build/test_load_image}"
OUT="${OUT:-/home/ubuntu/confirm}"; mkdir -p "$OUT"
base_changes="${BASE_IMAGE%.image}.changes"
src="$(ls "$(dirname "$BASE_IMAGE")"/*.sources 2>/dev/null | head -1)"
[ -n "$src" ] && cp -f "$src" "$OUT/" 2>/dev/null

fails() {  # $1=image ; env PHARO_X86_JIT controls JIT ; prints sorted FAIL/ERR set
    PHARO_MAX_STEPS=2000000000000 timeout 900 "$CUSTOM_VM" "$1" eval "$(cat "$RUNNER")" 2>&1 \
      | grep -aE '^(FAIL|ERR|TIMEOUT) ' | grep -avE '\[JIT\]' \
      | sed -E 's/ \[.*//; s/ \(.*//' | sort -u
}

while IFS=$'\t' read -r label load_expr prefix; do
    [ -n "$label" ] || continue
    img="$OUT/$label.image"
    cp -f "$BASE_IMAGE" "$img"; [ -f "$base_changes" ] && cp -f "$base_changes" "${img%.image}.changes"
    if ! timeout 600 "$PHARO" "$img" eval --save "$load_expr" >/dev/null 2>&1; then
        echo "$label: LOAD-FAILED"; rm -f "$img" "${img%.image}.changes"; continue
    fi
    export PKG_PREFIXES="$prefix"
    PHARO_X86_JIT=1 fails "$img" > "$OUT/$label.jiton"
    PHARO_X86_JIT=0 fails "$img" > "$OUT/$label.jitoff"
    jiton=$(wc -l < "$OUT/$label.jiton"); jitoff=$(wc -l < "$OUT/$label.jitoff")
    caused=$(comm -23 "$OUT/$label.jiton" "$OUT/$label.jitoff" | wc -l | tr -d ' ')
    verdict=$([ "$caused" -gt 0 ] && echo "*** REAL JIT BUG x$caused ***" || echo "vm-core/env (not JIT)")
    echo "$label: jit-caused=$caused (jit-on fails=$jiton, jit-off fails=$jitoff) -> $verdict"
    [ "$caused" -gt 0 ] && comm -23 "$OUT/$label.jiton" "$OUT/$label.jitoff" | sed 's/^/    JIT> /'
    rm -f "$img" "${img%.image}.changes"
done
