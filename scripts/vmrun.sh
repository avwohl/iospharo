#!/bin/bash
#
# vmrun.sh -- the one way to start a Pharo VM for a test on this machine.
#
# Rules that this script keeps (owner's rules of 2026-11-17):
#   1. One VM at a time.  A lock makes each call wait until the call before
#      it has ended.  The script refuses to start when another test VM is
#      alive that it did not start.
#   2. A VM is gone before the script returns.  After the run, the script
#      kills each process that still has the run directory in its command
#      line, and checks that none is left.
#   3. The machine keeps free memory.  The script does not start a VM when
#      free memory is low or swap is high, and a guard stops the VM during
#      the run when free memory gets low or the VM gets too large.
#
#   vmrun.sh status
#   vmrun.sh stop                          stop the VM of the running call
#   vmrun.sh test  <vm> <Class> [selector ...]
#   vmrun.sh eval  <vm> <file.st>
#   vmrun.sh prep  <vm> <base.image> <out.image> <file.st> [file.st ...]
#   vmrun.sh sweep <vm> <prepared.image> <outdir>
#   vmrun.sh run   <vm> <image> [image arguments ...]
#   vmrun.sh build <build-tree>            cmake --build under the same lock
#
# <vm> is a name or a path of a VM binary:
#   ours     build-rel/test_load_image           nojit  build-nojit/test_load_image
#   x86      build-x86/test_load_image           dbg    build-dbg/test_load_image
#   cog      stock Pharo 14 VM (/tmp/harness14)  cog13  stock Pharo 13 VM (/tmp/harness)
#   cogp     the Cog build with the changes of docs/upstream ($PHARO_WORK/cogbuild-all)
#
# Environment (all optional):
#   PHARO_WORK        work directory (default /tmp/pharo-work)
#   VMRUN_IMAGE       image for test and eval (default: $PHARO_WORK/images/<kind>.image)
#   VMRUN_TIMEOUT     seconds for the whole run (default 900; sweep 7200)
#   VMRUN_TEST_LIMIT  seconds for one test in `test` (default 120)
#   VMRUN_INTERACTIVE 1 = interactive session for `test` (a window opens on Cog)
#   VMRUN_MIN_FREE    do not start below this free-memory percentage (default 25)
#   VMRUN_KILL_FREE   stop the VM below this free-memory percentage (default 10)
#   VMRUN_MAX_SWAP_MB do not start above this swap use (default 4096)
#   VMRUN_MAX_RSS_MB  stop the VM above this resident size (default 10240)
#   VMRUN_WAIT        seconds to wait for the lock (default 3600)
#   VMRUN_OLD_SPACE_MB  old-space limit of our VM (default 6144)
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${PHARO_WORK:-/tmp/pharo-work}
LOCK=/tmp/pharo-vmrun.lock
VM_PATTERN='test_load_image|Pharo\.app/Contents/MacOS/Pharo'

MIN_FREE=${VMRUN_MIN_FREE:-25}
KILL_FREE=${VMRUN_KILL_FREE:-10}
MAX_SWAP_MB=${VMRUN_MAX_SWAP_MB:-4096}
MAX_RSS_MB=${VMRUN_MAX_RSS_MB:-10240}

say() { printf 'vmrun: %s\n' "$*" >&2; }

free_pct() { memory_pressure 2>/dev/null | awk -F': ' '/free percentage/ { gsub("%", "", $2); print int($2) }'; }
swap_mb()  { sysctl -n vm.swapusage 2>/dev/null | awk '{ for (i = 1; i <= NF; i++) if ($i == "used") { v = $(i + 2); u = substr(v, length(v)); n = v + 0; if (u == "G") n *= 1024; print int(n) } }'; }

vm_pids() { pgrep -f "$VM_PATTERN" 2>/dev/null; }

resolve_vm() {                      # sets VM (binary) and KIND (ours|cog)
    case "$1" in
        ours)  VM=$REPO/build-rel/test_load_image;   KIND=ours ;;
        nojit) VM=$REPO/build-nojit/test_load_image; KIND=ours ;;
        x86)   VM=$REPO/build-x86/test_load_image;   KIND=ours ;;
        dbg)   VM=$REPO/build-dbg/test_load_image;   KIND=ours ;;
        cog)   VM=/tmp/harness14/pharo-vm/Pharo.app/Contents/MacOS/Pharo; KIND=cog ;;
        cog13) VM=/tmp/harness/pharo-vm/Pharo.app/Contents/MacOS/Pharo;   KIND=cog ;;
        cogp)  VM=$WORK/cogbuild-all/build/vm/Debug/Pharo.app/Contents/MacOS/Pharo; KIND=cog ;;
        */test_load_image) VM=$1; KIND=ours ;;
        */Pharo)           VM=$1; KIND=cog ;;
        *) say "unknown VM: $1"; exit 2 ;;
    esac
    [ -x "$VM" ] || { say "no VM binary at $VM"; exit 2; }
}

lock_holder_alive() { [ -f "$LOCK/pid" ] && kill -0 "$(cat "$LOCK/pid" 2>/dev/null)" 2>/dev/null; }

acquire_lock() {                    # $1 = text for the status display
    local waited=0 limit=${VMRUN_WAIT:-3600}
    while ! mkdir "$LOCK" 2>/dev/null; do
        if ! lock_holder_alive; then
            rm -rf "$LOCK"          # the holder died without cleanup
            continue
        fi
        if [ "$waited" -ge "$limit" ]; then
            say "another call holds the lock for more than ${limit}s: $(cat "$LOCK/what" 2>/dev/null)"
            exit 75
        fi
        [ $((waited % 60)) -eq 0 ] && say "waiting for: $(cat "$LOCK/what" 2>/dev/null)"
        sleep 2; waited=$((waited + 2))
    done
    echo $$ > "$LOCK/pid"
    printf '%s\n' "$1" > "$LOCK/what"
    trap 'release_lock' EXIT
    trap 'cleanup_run; exit 130' INT TERM HUP
}

release_lock() {
    cleanup_run
    [ "$(cat "$LOCK/pid" 2>/dev/null)" = "$$" ] && rm -rf "$LOCK"
}

RUNTAG=""                           # a string that is in the command line of every process of this run
GUARD_PID=""

cleanup_run() {
    [ -n "$GUARD_PID" ] && kill "$GUARD_PID" 2>/dev/null
    GUARD_PID=""
    [ -n "$RUNTAG" ] || return 0
    local i pids
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pids=$(pgrep -f "$RUNTAG" 2>/dev/null | grep -v "^$$\$")
        [ -z "$pids" ] && break
        if [ "$i" -le 5 ]; then kill $pids 2>/dev/null; else kill -9 $pids 2>/dev/null; fi
        sleep 1
    done
    pids=$(pgrep -f "$RUNTAG" 2>/dev/null | grep -v "^$$\$")
    [ -n "$pids" ] && say "WARNING: still alive after kill -9: $pids"
    RUNTAG=""
}

check_memory_before_start() {
    local tries=0 f s
    while :; do
        f=$(free_pct); s=$(swap_mb)
        if [ "${f:-100}" -ge "$MIN_FREE" ] && [ "${s:-0}" -le "$MAX_SWAP_MB" ]; then return 0; fi
        tries=$((tries + 1))
        if [ "$tries" -gt 12 ]; then
            say "REFUSED: free memory ${f}% (need ${MIN_FREE}%), swap ${s} MB (limit ${MAX_SWAP_MB} MB)"
            exit 76
        fi
        say "memory is low (free ${f}%, swap ${s} MB): waiting"
        sleep 10
    done
}

check_no_foreign_vm() {
    local pids
    pids=$(vm_pids)
    if [ -n "$pids" ]; then
        say "REFUSED: a VM that this call did not start is alive:"
        ps -o pid=,rss=,etime=,command= -p $(echo $pids | tr ' ' ',') 2>/dev/null | cut -c1-160 >&2
        exit 77
    fi
}

memory_guard() {                    # runs beside the VM; $1 = run tag
    local f rss
    while sleep 5; do
        f=$(free_pct)
        rss=$(ps -axo rss=,command= | grep -F "$1" | grep -v grep | awk '{ s += $1 } END { print int(s / 1024) }')
        if [ "${f:-100}" -lt "$KILL_FREE" ] || [ "${rss:-0}" -gt "$MAX_RSS_MB" ]; then
            say "MEMORY-GUARD: free ${f}%, VM ${rss} MB: stopping the VM"
            echo "MEMORY-GUARD free=${f}% rss=${rss}MB" > "$LOCK/memory-guard"
            pgrep -f "$1" | grep -v "^$$\$" | xargs kill 2>/dev/null
            return
        fi
    done
}

# Run one command line that starts a VM.  $1 = run tag (a path that is in the
# command line), $2 = seconds, then the command.
guarded() {
    RUNTAG=$1; local limit=$2; shift 2
    check_no_foreign_vm
    check_memory_before_start
    memory_guard "$RUNTAG" &
    GUARD_PID=$!
    timeout -k 10 "$limit" "$@"
    local rc=$?
    cleanup_run
    [ -f "$LOCK/memory-guard" ] && { cat "$LOCK/memory-guard" >&2; rm -f "$LOCK/memory-guard"; rc=78; }
    return $rc
}

ours_env() {
    # No PHARO_EVAL_NO_SUNIT_MARKER here: sunit-sweep.sh asks a runner image for
    # its class count with an eval, and that eval must write the marker, or the
    # image starts the whole suite inside the eval.
    export PHARO_MAX_STEPS=4000000000000 PHARO_CODE_ZONE_MB=192
    export PHARO_MAX_OLD_SPACE_MB=${VMRUN_OLD_SPACE_MB:-6144}
}

default_image() {                   # $1 = vm name
    case "$1" in
        cog|cogp) echo "$WORK/images/cog14.image" ;;
        cog13)    echo "$WORK/images/cog13.image" ;;
        *)        echo "$WORK/images/ours14.image" ;;
    esac
}

fresh_rundir() {                    # sets RUNDIR with a copy of image $1
    local src=$1
    [ -f "$src" ] || { say "no image $src"; exit 2; }
    RUNDIR=$WORK/run/$$
    rm -rf "$RUNDIR"; mkdir -p "$RUNDIR"
    cp "$src" "$RUNDIR/rt.image" && cp "${src%.image}.changes" "$RUNDIR/rt.changes" || exit 2
    cp "$(dirname "$src")"/*.sources "$RUNDIR/" 2>/dev/null
    ls "$RUNDIR"/*.sources >/dev/null 2>&1 || { say "no .sources file beside $src"; exit 2; }
}

cmd=${1:-status}; [ $# -gt 0 ] && shift

case "$cmd" in
status)
    echo "free memory: $(free_pct)%   swap used: $(swap_mb) MB   load:$(uptime | sed 's/.*load averages*://')"
    if lock_holder_alive; then echo "lock: held by $(cat "$LOCK/pid"): $(cat "$LOCK/what" 2>/dev/null)"; else echo "lock: free"; fi
    pids=$(vm_pids)
    if [ -n "$pids" ]; then echo "VM processes:"; ps -o pid=,rss=,etime=,command= -p $(echo $pids | tr ' ' ',') | cut -c1-170; else echo "VM processes: none"; fi
    ;;

stop)
    if lock_holder_alive; then
        holder=$(cat "$LOCK/pid"); say "stopping call $holder: $(cat "$LOCK/what" 2>/dev/null)"
        kill "$holder" 2>/dev/null; sleep 3
    fi
    pids=$(vm_pids); [ -n "$pids" ] && { say "VM processes still alive: $pids"; exit 1; }
    echo "no VM process"
    ;;

test)
    name=${1:?usage: vmrun.sh test <vm> <Class> [selector ...]}; cls=${2:?class}; shift 2
    resolve_vm "$name"
    acquire_lock "test $name $cls"
    fresh_rundir "${VMRUN_IMAGE:-$(default_image "$name")}"
    cd "$RUNDIR" || exit 2
    { echo "$cls"; for s in "$@"; do echo "$s"; done; echo "TIMEOUT=${VMRUN_TEST_LIMIT:-120}"; } > runtest_args.txt
    expr="'$REPO/scripts/vmrun/one_test.st' asFileReference fileIn"
    headless="--headless"
    if [ "${VMRUN_INTERACTIVE:-0}" = "1" ]; then export PHARO_INTERACTIVE=1; headless=""; fi
    if [ "$KIND" = ours ]; then
        ours_env
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
    else
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" $headless "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
    fi
    rc=$?
    if [ -f runtest_out.txt ]; then cat runtest_out.txt; else echo "NO-OUTPUT rc=$rc"; tail -5 vm.log; fi
    grep -q '^DONE' runtest_out.txt 2>/dev/null || echo "INCOMPLETE rc=$rc (log: $RUNDIR/vm.log)"
    ;;

eval)
    name=${1:?usage: vmrun.sh eval <vm> <file.st>}; file=${2:?file}
    file=$(cd "$(dirname "$file")" && pwd)/$(basename "$file")
    resolve_vm "$name"
    acquire_lock "eval $name $(basename "$file")"
    fresh_rundir "${VMRUN_IMAGE:-$(default_image "$name")}"
    cd "$RUNDIR" || exit 2
    expr="| r | r := [ (Smalltalk compiler evaluate: '$file' asFileReference contents) displayString ] on: Error do: [ :e | 'EVAL-ERROR ' , e class name , ': ' , (e messageText ifNil: [ '' ]) ]. 'eval_out.txt' asFileReference writeStreamDo: [ :o | o nextPutAll: r ]. r size"
    if [ "$KIND" = ours ]; then
        ours_env
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-300}" "$VM" "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
    else
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-300}" "$VM" --headless "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
    fi
    rc=$?
    if [ -f eval_out.txt ]; then tr '\r' '\n' < eval_out.txt; echo; else echo "NO-OUTPUT rc=$rc (log: $RUNDIR/vm.log)"; tail -5 vm.log; fi
    ;;

prep)
    name=${1:?usage: vmrun.sh prep <vm> <base.image> <out.image> <file.st> ...}; base=${2:?base image}; out=${3:?out image}; shift 3
    [ $# -gt 0 ] || { say "no file to file in"; exit 2; }
    resolve_vm "$name"
    acquire_lock "prep $name $(basename "$out")"
    fresh_rundir "$base"
    cd "$RUNDIR" || exit 2
    expr=""
    for f in "$@"; do
        f=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
        [ -f "$f" ] || { say "no file $f"; exit 2; }
        expr="$expr'$f' asFileReference fileIn. "
    done
    if [ "$KIND" = ours ]; then
        ours_env
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" "$RUNDIR/rt.image" eval "${expr}Smalltalk snapshot: true andQuit: true" > vm.log 2>&1 < /dev/null
    else
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" --headless "$RUNDIR/rt.image" eval --save "${expr}1" > vm.log 2>&1 < /dev/null
    fi
    rc=$?
    grep -a -o '\(FIXED\|ADDED\|SKIP\) [A-Za-z0-9 ]*>>[A-Za-z0-9_:]*' vm.log | sort -u | awk '{ print $1 }' | sort | uniq -c | tr '\n' ' '; echo
    grep -a -i 'OCSyntaxError\|Error:' vm.log | grep -v '^\[' | cut -c1-200 | head -5
    if cmp -s "$base" "$RUNDIR/rt.image"; then say "the image did not change: the prep failed (log: $RUNDIR/vm.log)"; exit 1; fi
    mkdir -p "$(dirname "$out")"
    cp "$RUNDIR/rt.image" "$out" && cp "$RUNDIR/rt.changes" "${out%.image}.changes" && cp "$RUNDIR"/*.sources "$(dirname "$out")/"
    ls -la "$out"
    echo "prep rc=$rc"
    ;;

sweep)
    name=${1:?usage: vmrun.sh sweep <vm> <prepared.image> <outdir>}; image=${2:?image}; outdir=${3:?outdir}
    resolve_vm "$name"
    [ -f "$image" ] || { say "no image $image"; exit 2; }
    acquire_lock "sweep $name $(basename "$outdir")"
    mkdir -p "$outdir"
    outdir=$(cd "$outdir" && pwd)
    export SUNIT_PREFIX=$outdir/state
    cp "$REPO/scripts/pharo-headless-test/test_classes.txt" "${SUNIT_PREFIX}_test_classes.txt"
    if [ "$KIND" = ours ]; then
        ours_env
        export SUNIT_TIMEOUT_MULT=${SUNIT_TIMEOUT_MULT:-1} STEP=${STEP:-50} PER_BATCH_TIMEOUT=${PER_BATCH_TIMEOUT:-3600} RETRY_DAMAGED=1 RETRY_STEP=5
        guarded "$outdir/" "${VMRUN_TIMEOUT:-7200}" "$REPO/scripts/sunit-sweep.sh" "$VM" "$image" "$outdir" > "$outdir/driver.log" 2>&1 < /dev/null
        rc=$?
    else
        # cog-sweep.sh runs <dir>/pharo <image>: give it a directory with this VM.
        cogdir=$outdir/cogdir; rm -rf "$cogdir"; mkdir -p "$cogdir"
        printf '#!/bin/bash\nexec "%s" --headless "$@"\n' "$VM" > "$cogdir/pharo"; chmod +x "$cogdir/pharo"
        cp "$image" "$cogdir/Pharo.image" && cp "${image%.image}.changes" "$cogdir/Pharo.changes" && cp "$(dirname "$image")"/*.sources "$cogdir/"
        list=${VMRUN_CLASS_LIST:-$WORK/classes14.txt}
        [ -f "$list" ] || { say "no class list $list (set VMRUN_CLASS_LIST)"; exit 2; }
        export COG_ARCH=native STEP=${STEP:-100} PER_BATCH_TIMEOUT=${PER_BATCH_TIMEOUT:-2400}
        guarded "$outdir/" "${VMRUN_TIMEOUT:-7200}" "$REPO/scripts/cog-sweep.sh" "$list" "$outdir" "$cogdir" > "$outdir/driver.log" 2>&1 < /dev/null
        rc=$?
    fi
    tail -n 14 "$outdir/sweep.log" 2>/dev/null
    echo "sweep rc=$rc"
    ;;

run)
    name=${1:?usage: vmrun.sh run <vm> <image> [image arguments ...]}; image=${2:?image}; shift 2
    resolve_vm "$name"
    acquire_lock "run $name $(basename "$image")"
    fresh_rundir "$image"
    cd "$RUNDIR" || exit 2
    if [ "$KIND" = ours ]; then
        ours_env
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" "$RUNDIR/rt.image" "$@" < /dev/null
    else
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" --headless "$RUNDIR/rt.image" "$@" < /dev/null
    fi
    echo "run rc=$? (directory: $RUNDIR)"
    ;;

build)
    tree=${1:?usage: vmrun.sh build <build-tree>}
    [ -d "$REPO/$tree" ] || { say "no build tree $REPO/$tree"; exit 2; }
    acquire_lock "build $tree"
    check_memory_before_start
    cmake --build "$REPO/$tree" -j4 2>&1 | grep -E ' error |FAILED|warning: unused' | head -20
    ls -la "$REPO/$tree/test_load_image"
    ;;

*)
    sed -n '3,45p' "${BASH_SOURCE[0]}"
    exit 2
    ;;
esac
