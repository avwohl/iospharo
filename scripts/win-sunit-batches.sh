#!/usr/bin/env bash
# Run the full set of non-abstract TestCase subclasses on Windows in
# timeout-protected batches (synchronous TestSuite run via eval). A batch that
# contains a hanger times out and is recorded as TIMEOUT; the rest complete.
# Aggregates pass/fail/error counts. Run from native Git Bash (USERPROFILE set).
set -u
EXE=/c/temp/src/iospharo-jit/build-win/test_load_image.exe
IMG="C:/temp/pharo-win-test/Pharo.image"
LIST=/c/tmp/tc_names.txt
OUT=/c/tmp/win_sunit_batches.txt
BATCH=${BATCH:-100}
PERTIMEOUT=${PERTIMEOUT:-200}
SKIP="ProcessTest WeakArrayTest"   # known hangers (documented in deferred.md)

: > "$OUT"
mapfile -t ALL < "$LIST"
# filter skips
NAMES=()
for n in "${ALL[@]}"; do
  skip=0; for s in $SKIP; do [ "$n" = "$s" ] && skip=1; done
  [ $skip -eq 0 ] && NAMES+=("$n")
done
total=${#NAMES[@]}
echo "Running $total classes (skipped: $SKIP) in batches of $BATCH" | tee -a "$OUT"

i=0; bn=0
while [ $i -lt $total ]; do
  chunk=("${NAMES[@]:$i:$BATCH}")
  syms=$(printf '%s ' "${chunk[@]}")
  expr="| s | s := TestSuite new. #($syms) do: [:nm | (Smalltalk at: nm ifAbsent: [nil]) ifNotNil: [:c | s addTests: c suite tests]]. s run printString"
  res=$(timeout $PERTIMEOUT "$EXE" "$IMG" eval "$expr" 2>&1 | grep -oE "EVAL-RESULT=[^[]*" | head -1 | sed 's/EVAL-RESULT=//')
  bn=$((bn+1))
  if [ -z "$res" ]; then
    echo "batch $bn [$i..$((i+BATCH-1))]: TIMEOUT/HANG (classes ${chunk[0]}..${chunk[-1]})" | tee -a "$OUT"
  else
    echo "batch $bn [$i..$((i+BATCH-1))]: $res" | tee -a "$OUT"
  fi
  i=$((i+BATCH))
done

echo "=== AGGREGATE ===" | tee -a "$OUT"
awk -F"'" '/ran,/ {print $2}' "$OUT" | awk '
  { for(j=1;j<=NF;j++){
      if($j=="ran,")  ran+=$(j-1);
      if($j=="passed,")pass+=$(j-1);
      if($j=="failures,")fail+=$(j-1);
      if($j=="errors,") err+=$(j-1);
      if($j=="skipped,")skip+=$(j-1);
    } }
  END { printf "TOTAL: %d ran, %d passed, %d skipped, %d failures, %d errors\n", ran, pass, skip, fail, err }' | tee -a "$OUT"
grep -c "TIMEOUT" "$OUT" | sed 's/^/timeout batches: /' | tee -a "$OUT"
