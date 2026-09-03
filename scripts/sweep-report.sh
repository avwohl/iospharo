#!/bin/bash
# Aggregate a finished (or running) sweep dir.  Usage: sweep-report.sh <outdir>
# Field map for a sweep.log batch line -- getting this wrong once made a clean
# run look like every batch had failed:
#   $1=batch  $2=<start>-<end>  $3=rc=N  $4=<secs>s  $5=classes=N  $6=completed=yes
D=${1:?usage: sweep-report.sh <sweep-outdir>}
echo "=== $D"
# rc=137 with completed=yes is the sweep's OWN early kill: the batch wrote the
# completion marker and the VM then sat wedged in shutdown, so run_batch killed
# it after SHUTDOWN_GRACE instead of burning the whole per-batch timeout.  That
# is the mechanism working, not a failure -- counting it as one made a healthy
# x86 run look like 4 bad batches of 13.
awk '/^batch/{n++; split($3,r,"=");
      if (r[2]==137 && $6=="completed=yes") early++
      else if (r[2]!=0) bad++}
     END{printf "--- batches: %d run, %d with rc!=0, %d killed early after finishing\n",
                n, bad+0, early+0}' "$D/sweep.log"
grep -a "^batch" "$D/sweep.log" | awk '{split($3,r,"=");
    if (r[2]!=0 && !(r[2]==137 && $6=="completed=yes")) print "  "$0}'
echo "--- batches that lost classes (classes= below the batch size)"
grep -a "^batch" "$D/sweep.log" | awk '{split($2,b,"-"); want=b[2]-b[1]+1;
    split($5,c,"="); if (c[2]+0 < want) print "  "$0}'
echo "--- totals (Total: N P:n F:n E:n S:n [T:n])"
for f in all_results.txt retry_results.txt; do
  [ -s "$D/$f" ] || continue
  tr '\r' '\n' < "$D/$f" | awk -v tag="$f" '
    /^Total:/ { for (i=3;i<=NF;i++) { split($i,kv,":"); t[kv[1]]+=kv[2] } n++ }
    END { printf "  %-18s classes=%d P=%d F=%d E=%d S=%d T=%d\n",
                 tag, n, t["P"], t["F"], t["E"], t["S"], t["T"] }'
done
echo "--- non-clean classes (F, E or T non-zero)"
# T counts.  An earlier version keyed on F and E only, and TIMEOUTs -- which
# ARE failures for the goal -- were invisible in the class list while still
# showing in the totals.
tr '\r' '\n' < "$D/all_results.txt" 2>/dev/null | awk '
  /^=== / { cls=$2 }
  /^Total:/ { split($4,f,":"); split($5,e,":"); t=0;
              for (i=6;i<=NF;i++) { split($i,kv,":"); if (kv[1]=="T") t=kv[2] }
              if (f[2]+0>0 || e[2]+0>0 || t+0>0) printf "  %-45s %s\n", cls, $0 }'
echo "--- defect #23 instrumentation"
for pat in CANNOT-RETURN-STORM DEAD-SENDER DOUBLE-RETURN DUP-FRAME "FATAL: old space"; do
  n=$(cat "$D"/batch_*.log "$D"/retry_*.log 2>/dev/null | grep -ac "$pat")
  r=$(grep -al "$pat" "$D"/batch_*.log "$D"/retry_*.log 2>/dev/null | wc -l | tr -d ' ')
  printf "  %-22s %6s lines across %s logs\n" "$pat" "$n" "$r"
done
