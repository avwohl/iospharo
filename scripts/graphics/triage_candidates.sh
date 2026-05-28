#!/bin/bash
# Classify Δcog candidates by isolation repro on the clean image (JIT on/off).
# For each tag, reads /tmp/cand/<tag>.txt (Class>>sel per line), runs them in
# isolation with JIT on and off, joins the two runs, and classifies each:
#   on=FAIL off=PASS  -> JIT-BUG     (fails only with JIT)
#   on=FAIL off=FAIL  -> VM-BUG      (interpreter/primitive; not JIT)
#   on=PASS           -> ARTIFACT    (passes in isolation; gfx/suite-state)
# Writes docs/results/jitpkg/<tag>_isolation.txt and a one-line tally.
set -u
REPRO=/Users/wohl/src/iospharo/scripts/repro_tests.sh
IMG="${IMG:-/tmp/harness/Pharo.image}"
OUTDIR=/Users/wohl/src/iospharo/docs/results/jitpkg

for tag in "$@"; do
    cand="/tmp/cand/${tag}.txt"
    [ -s "$cand" ] || { echo "$tag: no candidates"; continue; }
    specs=()
    while IFS= read -r line; do [ -n "$line" ] && specs+=("$line"); done < "$cand"
    "$REPRO" "$IMG" "${specs[@]}" >/dev/null 2>&1
    out="$OUTDIR/${tag}_isolation.txt"
    jitbug=0; vmbug=0; artifact=0
    : > "$out"
    while IFS= read -r spec; do
        [ -z "$spec" ] && continue
        on=$(grep -F "$spec =>" /tmp/repro_jit.txt   | head -1 | sed 's/.*=> //')
        off=$(grep -F "$spec =>" /tmp/repro_nojit.txt | head -1 | sed 's/.*=> //')
        on=${on:-MISSING}; off=${off:-MISSING}
        cls=ARTIFACT
        if [[ "$on" == FAIL* && "$off" == PASS ]]; then cls=JIT-BUG; jitbug=$((jitbug+1))
        elif [[ "$on" == FAIL* && "$off" == FAIL* ]]; then cls=VM-BUG; vmbug=$((vmbug+1))
        elif [[ "$on" == PASS ]]; then cls=ARTIFACT; artifact=$((artifact+1))
        else cls=OTHER; fi
        printf '%-9s %s\n    on : %s\n    off: %s\n' "$cls" "$spec" "$on" "$off" >> "$out"
    done < "$cand"
    printf '%-12s JIT-BUG=%-3s VM-BUG=%-3s ARTIFACT=%-3s  -> %s\n' \
        "$tag" "$jitbug" "$vmbug" "$artifact" "$out"
done
