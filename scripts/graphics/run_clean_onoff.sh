#!/bin/bash
# Gold-standard JIT-bug test: run each package's FULL suite on the clean harness
# image (Pharo-jit, no FakeGUI) with JIT ON and JIT OFF. Tests that fail JIT-on
# but pass JIT-off are JIT-induced (the aigraph signature). This eliminates both
# the gfx-image startup corruption AND the stock-Cog version confound — the only
# variable is JIT on vs off on the same image.
set -u
IMAGE="${IMAGE:-/tmp/harness/Pharo-jit.image}"
VM="${VM:-/Users/wohl/src/iospharo/build/test_load_image}"
OUTDIR="${OUTDIR:-/Users/wohl/src/iospharo/docs/results/jitpkg}"
TO="${TO:-900}"

run_side() {  # $1=tag $2=label $3=env
    cp "/tmp/pkg_${1}.txt" /tmp/sunit_class_names.txt
    rm -f /tmp/sunit_run_completed.txt /tmp/sunit_test_results.txt /tmp/sunit_test_detail.txt
    env $3 timeout "$TO" "$VM" "$IMAGE" > "$OUTDIR/${1}_clean_${2}.log" 2>&1
    cp /tmp/sunit_test_detail.txt "$OUTDIR/${1}_clean_${2}.txt" 2>/dev/null
    cp /tmp/sunit_test_results.txt "$OUTDIR/${1}_clean_${2}_results.txt" 2>/dev/null
}

for tag in "$@"; do
    [ -f "/tmp/pkg_${tag}.txt" ] || { echo "$tag: no list"; continue; }
    run_side "$tag" jit   ""
    run_side "$tag" nojit "PHARO_NO_JIT=1"
    python3 - "$tag" "$OUTDIR" <<'PY'
import sys
tag, outdir = sys.argv[1], sys.argv[2]
def parse(p):
    d={}
    try:
        for ln in open(p):
            f=ln.rstrip("\n").split("\t")
            if len(f)==4: d[(f[1],f[2])]=f[3]
    except FileNotFoundError: pass
    return d
on=parse(f"{outdir}/{tag}_clean_jit.txt"); off=parse(f"{outdir}/{tag}_clean_nojit.txt")
def c(d):
    from collections import Counter; return dict(Counter(d.values()))
jitbugs=[k for k,v in on.items() if v!="PASS" and off.get(k)=="PASS"]
bothfail=[k for k,v in on.items() if v!="PASS" and off.get(k) not in (None,"PASS")]
print(f"{tag:12} on={c(on)} off={c(off)}  JIT-INDUCED={len(jitbugs)} both-fail={len(bothfail)}")
for k in sorted(jitbugs): print(f"    JIT-BUG  {k[0]}>>{k[1]}  (on={on[k]} off=PASS)")
PY
done
