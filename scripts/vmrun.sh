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
#   vmrun.sh sweep <vm> <image> <outdir>   the image has the fake GUI and the
#                                          image fixes, and no runner inside
#   vmrun.sh interactive <vm> <image> <outdir>   scripts/interactive-classes.txt
#                                          in an interactive session
#   vmrun.sh pkgs [--redo|--reuse] <outdir> <first-row> [last-row] [ours-vm] [cog-vm]
#                                          packages of scripts/pkg-jit-test/
#                                          packages-200.tsv: load from GitHub
#                                          on Cog, tests on Cog and on our VM
#   vmrun.sh run   <vm> <image> [image arguments ...]
#   vmrun.sh build <build-tree>            cmake --build under the same lock
#   vmrun.sh cogbuild                      build the Cog tree with the changes of
#                                          docs/upstream and put its libraries
#                                          into the app of the VM name cogp
#
# <vm> is a name or a path of a VM binary:
#   ours     build-rel/test_load_image           nojit  build-nojit/test_load_image
#   x86      build-x86/test_load_image           dbg    build-dbg/test_load_image
#   cog      stock Pharo 14 VM (/tmp/harness14)  cog13  stock Pharo 13 VM (/tmp/harness)
#   cog15    stock Pharo 15 VM (/tmp/harness15)
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

# A VM gets no key, token or password of the user.  An image can hold the
# code of other people (the packages of the package run), and that code can
# read the environment.  Measured 2026-11-17: the tests of one package read
# an API key from the environment and sent requests with it.
for v in $(compgen -e | grep -E -i 'KEY|TOKEN|SECRET|PASSW|CREDENTIAL|^ANTHROPIC_|^CLAUDE|^GH_|^GITHUB_|^AWS_|^OPENAI_|^GOOGLE_'); do
    unset "$v"
done

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${PHARO_WORK:-/tmp/pharo-work}
# The lock is a symbolic link whose text is the process id of the holder.
# ln -s makes the link and the id in one step, so no call can see a lock that
# has no holder yet.  $LOCK.what has the text for the status display.
LOCK=/tmp/pharo-vmrun.lck
VM_PATTERN='test_load_image|Pharo\.app/Contents/MacOS/Pharo'

MIN_FREE=${VMRUN_MIN_FREE:-25}
KILL_FREE=${VMRUN_KILL_FREE:-10}
MAX_SWAP_MB=${VMRUN_MAX_SWAP_MB:-4096}
MAX_RSS_MB=${VMRUN_MAX_RSS_MB:-10240}

say() { printf 'vmrun: %s\n' "$*" >&2; }

free_pct() { memory_pressure 2>/dev/null | awk -F': ' '/free percentage/ { gsub("%", "", $2); print int($2) }'; }
swap_mb()  { sysctl -n vm.swapusage 2>/dev/null | awk '{ for (i = 1; i <= NF; i++) if ($i == "used") { v = $(i + 2); u = substr(v, length(v)); n = v + 0; if (u == "G") n *= 1024; print int(n) } }'; }

# The processes whose PROGRAM is a VM.  pgrep -f looks at the whole command
# line, so a shell with the path of a VM in its text (a script that someone
# started with "sh -c '... test_load_image ...'") matched too.  On 2026-11-17
# such a shell was "a VM that this call did not start" for 6 minutes, and
# each other call was refused: the package loop stopped at row 86.
vm_pids() {
    local p
    for p in $(pgrep -f "$VM_PATTERN" 2>/dev/null); do
        ps -o comm= -p "$p" 2>/dev/null | grep -Eq "$VM_PATTERN" && echo "$p"
    done
}

resolve_vm() {                      # sets VM (binary) and KIND (ours|cog)
    case "$1" in
        ours)  VM=$REPO/build-rel/test_load_image;   KIND=ours ;;
        nojit) VM=$REPO/build-nojit/test_load_image; KIND=ours ;;
        x86)   VM=$REPO/build-x86/test_load_image;   KIND=ours ;;
        dbg)   VM=$REPO/build-dbg/test_load_image;   KIND=ours ;;
        cog)   VM=/tmp/harness14/pharo-vm/Pharo.app/Contents/MacOS/Pharo; KIND=cog ;;
        cog13) VM=/tmp/harness/pharo-vm/Pharo.app/Contents/MacOS/Pharo;   KIND=cog ;;
        cog15) VM=/tmp/harness15/pharo-vm/Pharo.app/Contents/MacOS/Pharo; KIND=cog ;;
        cogp)  # the stock app with the VM core and the socket plugin of the build with
               # the changes of docs/upstream (a local Cog build lacks freetype, cairo
               # and libgit2, so the stock app keeps its own third-party libraries)
               VM=$WORK/cogp-app/Pharo.app/Contents/MacOS/Pharo; KIND=cog
               # This build runs a callback from another thread in the VM
               # thread (scripts/image-fixes/30-ffi-callback-from-other-thread.st).
               export PHARO_FFI_CALLBACK_FROM_OTHER_THREAD=1 ;;
        */test_load_image) VM=$1; KIND=ours ;;
        */Pharo)           VM=$1; KIND=cog ;;
        *) say "unknown VM: $1"; exit 2 ;;
    esac
    [ -x "$VM" ] || { say "no VM binary at $VM"; exit 2; }
}

lock_holder() { readlink "$LOCK" 2>/dev/null; }
lock_holder_alive() { local h; h=$(lock_holder); [ -n "$h" ] && kill -0 "$h" 2>/dev/null; }

acquire_lock() {                    # $1 = text for the status display
    local waited=0 limit=${VMRUN_WAIT:-3600} h
    while ! ln -s "$$" "$LOCK" 2>/dev/null; do
        h=$(lock_holder)
        if [ -n "$h" ] && ! kill -0 "$h" 2>/dev/null; then
            # The holder died without cleanup.  mv is atomic: one call wins.
            mv "$LOCK" "$LOCK.dead.$$" 2>/dev/null && rm -f "$LOCK.dead.$$"
            continue
        fi
        if [ "$waited" -ge "$limit" ]; then
            say "another call holds the lock for more than ${limit}s: $(cat "$LOCK.what" 2>/dev/null)"
            exit 75
        fi
        [ $((waited % 60)) -eq 0 ] && say "waiting for: $(cat "$LOCK.what" 2>/dev/null)"
        sleep 2; waited=$((waited + 2))
    done
    printf '%s\n' "$1" > "$LOCK.what"
    rm -f "$LOCK.memory-guard"
    trap 'release_lock' EXIT
    trap 'cleanup_run; exit 130' INT TERM HUP
}

release_lock() {
    cleanup_run
    if [ "$(lock_holder)" = "$$" ]; then rm -f "$LOCK" "$LOCK.what" "$LOCK.memory-guard"; fi
}

RUNTAG=""                           # a string that is in the command line of every process of this run
GUARD_PID=""
CMD_PID=""                          # the command that guarded() started

# The processes of a run: each process that has the run tag in its command
# line and is a VM, a timeout that started a VM, or a script that starts VMs.
# A tail, a grep or an editor on a file of the run directory is not one.
tagged_pids() {                     # $1 = run tag
    local p
    for p in $(pgrep -f "$1" 2>/dev/null); do
        [ "$p" = "$$" ] && continue
        ps -o command= -p "$p" 2>/dev/null \
            | grep -Eq "$VM_PATTERN|^([^ ]*/)?timeout |sunit-sweep\.sh|cog-sweep\.sh|run-one-package\.sh" && echo "$p"
    done
}

cleanup_run() {
    [ -n "$GUARD_PID" ] && kill "$GUARD_PID" 2>/dev/null
    GUARD_PID=""
    [ -n "$CMD_PID" ] && kill "$CMD_PID" 2>/dev/null
    CMD_PID=""
    [ -n "$RUNTAG" ] || return 0
    local i pids
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pids=$(tagged_pids "$RUNTAG")
        [ -z "$pids" ] && break
        if [ "$i" -le 5 ]; then kill $pids 2>/dev/null; else kill -9 $pids 2>/dev/null; fi
        sleep 1
    done
    pids=$(tagged_pids "$RUNTAG")
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
    # The guard does not stop after the first kill: a sweep or a package run
    # starts more VMs after the one that the guard stopped.
    local f rss p
    while sleep 5; do
        f=$(free_pct)
        rss=$(for p in $(tagged_pids "$1"); do ps -o rss= -p "$p" 2>/dev/null; done | awk '{ s += $1 } END { print int(s / 1024) }')
        if [ "${f:-100}" -lt "$KILL_FREE" ] || [ "${rss:-0}" -gt "$MAX_RSS_MB" ]; then
            say "MEMORY-GUARD: free ${f}%, VM ${rss} MB: stopping the VM"
            echo "MEMORY-GUARD free=${f}% rss=${rss}MB" >> "$LOCK.memory-guard"
            # The VM processes only: the script that starts the VMs goes on.
            for p in $(tagged_pids "$1"); do
                ps -o comm= -p "$p" 2>/dev/null | grep -Eq "$VM_PATTERN" && kill "$p" 2>/dev/null
            done
            sleep 10
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
    # In the background, with a wait: bash runs the INT/TERM trap at once
    # during a wait, and only after the end of a command in the foreground.
    timeout -k 10 "$limit" "$@" &
    CMD_PID=$!
    wait "$CMD_PID"
    local rc=$?
    CMD_PID=""
    cleanup_run
    [ -f "$LOCK.memory-guard" ] && { tail -n 1 "$LOCK.memory-guard" >&2; rm -f "$LOCK.memory-guard"; rc=78; }
    return $rc
}

# Stock Cog gets the old-space limit of our VM (see ours_env).
cog_mem() { echo "--maxOldSpaceSize=${VMRUN_OLD_SPACE_MB:-6144}M"; }
# A VM error of stock Cog prints the stack of each process: at most 200 frames.
COG_FRAMES=--maxFramesToLog=200

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
        cog15)    echo "$WORK/images/cog15.image" ;;
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

# Options after the subcommand (the same as the environment variables, so a
# call needs no variable assignment in front of it):
#   --image <path>   VMRUN_IMAGE        --interactive   VMRUN_INTERACTIVE=1
#   --timeout <s>    VMRUN_TIMEOUT      --limit <s>     VMRUN_TEST_LIMIT
while [ $# -gt 0 ]; do
    case "$1" in
        --image)       VMRUN_IMAGE=$2; shift 2 ;;
        --interactive) VMRUN_INTERACTIVE=1; shift ;;
        --timeout)     VMRUN_TIMEOUT=$2; shift 2 ;;
        --limit)       VMRUN_TEST_LIMIT=$2; shift 2 ;;
        --repeat)      VMRUN_REPEAT=$2; shift 2 ;;   # test: N runs, one VM after the other, counts at the end
        --old-space)   VMRUN_OLD_SPACE_MB=$2; shift 2 ;;   # old-space limit in MB, for our VM and for stock Cog
        --from)        VMRUN_FROM=$2; shift 2 ;;     # sweep of our VM: first class index
        --to)          VMRUN_TO=$2; shift 2 ;;       # sweep of our VM: last class index; no retry pass, no interactive pass
        --redo)        VMRUN_REDO=1; shift ;;        # pkgs: run a package again that has a row
        --reuse)       VMRUN_REDO=1; VMRUN_REUSE=1; shift ;;   # pkgs: again, from the image of the earlier load
        *) break ;;
    esac
done

case "$cmd" in
status)
    echo "free memory: $(free_pct)%   swap used: $(swap_mb) MB   load:$(uptime | sed 's/.*load averages*://')"
    if lock_holder_alive; then echo "lock: held by $(lock_holder): $(cat "$LOCK.what" 2>/dev/null)"; else echo "lock: free"; fi
    pids=$(vm_pids)
    if [ -n "$pids" ]; then echo "VM processes:"; ps -o pid=,rss=,etime=,command= -p $(echo $pids | tr ' ' ',') | cut -c1-170; else echo "VM processes: none"; fi
    ;;

stop)
    if lock_holder_alive; then
        holder=$(lock_holder); say "stopping call $holder: $(cat "$LOCK.what" 2>/dev/null)"
        kill "$holder" 2>/dev/null
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do kill -0 "$holder" 2>/dev/null || break; sleep 1; done
    fi
    pids=$(vm_pids); [ -n "$pids" ] && { say "VM processes still alive: $pids"; exit 1; }
    echo "no VM process"
    ;;

test)
    name=${1:?usage: vmrun.sh test <vm> <Class> [selector ...]}; cls=${2:?class}; shift 2
    resolve_vm "$name"
    acquire_lock "test $name $cls"
    repeat=${VMRUN_REPEAT:-1}
    summary=$WORK/run/$$.summary; mkdir -p "$WORK/run"; : > "$summary"
    headless="--headless"
    if [ "${VMRUN_INTERACTIVE:-0}" = "1" ]; then export PHARO_INTERACTIVE=1; headless=""; fi
    expr="'$REPO/scripts/vmrun/one_test.st' asFileReference fileIn"
    n=1
    while [ "$n" -le "$repeat" ]; do
        fresh_rundir "${VMRUN_IMAGE:-$(default_image "$name")}"
        cd "$RUNDIR" || exit 2
        { echo "$cls"; for s in "$@"; do echo "$s"; done; echo "TIMEOUT=${VMRUN_TEST_LIMIT:-120}"; } > runtest_args.txt
        if [ "$KIND" = ours ]; then
            ours_env
            guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
        else
            guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" $headless "$(cog_mem)" "$COG_FRAMES" "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
        fi
        rc=$?
        if [ "$repeat" -eq 1 ]; then
            if [ -f runtest_out.txt ]; then cat runtest_out.txt; else echo "NO-OUTPUT rc=$rc"; tail -5 vm.log; fi
            grep -q '^DONE' runtest_out.txt 2>/dev/null || echo "INCOMPLETE rc=$rc (log: $RUNDIR/vm.log)"
        else
            # one line per test and run, without the time, for the counts
            if [ -f runtest_out.txt ]; then sed -E 's/ [0-9]+ms( |$)/ /' runtest_out.txt | grep -v '^DONE' >> "$summary"; fi
            grep -q '^DONE' runtest_out.txt 2>/dev/null || echo "INCOMPLETE rc=$rc" >> "$summary"
        fi
        n=$((n + 1))
    done
    if [ "$repeat" -gt 1 ]; then echo "$repeat runs:"; cut -c1-200 "$summary" | sort | uniq -c; fi
    rm -f "$summary"
    ;;

eval)
    name=${1:?usage: vmrun.sh eval <vm> <file.st>}; file=${2:?file}
    file=$(cd "$(dirname "$file")" && pwd)/$(basename "$file")
    resolve_vm "$name"
    acquire_lock "eval $name $(basename "$file")"
    fresh_rundir "${VMRUN_IMAGE:-$(default_image "$name")}"
    cd "$RUNDIR" || exit 2
    # A String answer goes out whole: displayString cuts a long String.
    expr="| r | r := [ | v | v := Smalltalk compiler evaluate: '$file' asFileReference contents. v isString ifTrue: [ v ] ifFalse: [ v displayString ] ] on: Error do: [ :e | 'EVAL-ERROR ' , e class name , ': ' , (e messageText ifNil: [ '' ]) ]. 'eval_out.txt' asFileReference writeStreamDo: [ :o | o nextPutAll: r ]. r size"
    if [ "$KIND" = ours ]; then
        ours_env
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-300}" "$VM" "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
    else
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-300}" "$VM" --headless "$(cog_mem)" "$COG_FRAMES" "$RUNDIR/rt.image" eval "$expr" > vm.log 2>&1 < /dev/null
    fi
    rc=$?
    if [ -f eval_out.txt ]; then tr '\r' '\n' < eval_out.txt; echo; else echo "NO-OUTPUT rc=$rc (log: $RUNDIR/vm.log)"; tail -5 vm.log; fi
    ;;

prep)
    name=${1:?usage: vmrun.sh prep <vm> <base.image> <out.image> <file.st> ...}; base=${2:?base image}; out=${3:?out image}; shift 3
    [ $# -gt 0 ] || { say "no file to file in"; exit 2; }
    resolve_vm "$name"
    # Paths can be relative to the directory of the caller: make them absolute
    # before the change to the run directory.
    expr=""
    for f in "$@"; do
        [ -f "$f" ] || { say "no file $f"; exit 2; }
        f=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
        expr="$expr'$f' asFileReference fileIn. "
    done
    case "$out" in /*) ;; *) out=$PWD/$out ;; esac
    acquire_lock "prep $name $(basename "$out")"
    fresh_rundir "$base"
    cd "$RUNDIR" || exit 2
    if [ "$KIND" = ours ]; then
        ours_env
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" "$RUNDIR/rt.image" eval "${expr}Smalltalk snapshot: true andQuit: true" > vm.log 2>&1 < /dev/null
    else
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" --headless "$(cog_mem)" "$COG_FRAMES" "$RUNDIR/rt.image" eval --save "${expr}1" > vm.log 2>&1 < /dev/null
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
        # A part of the class list (to run a batch again that stopped): no
        # retry pass, so a batch that stops shows as stopped.
        if [ -n "${VMRUN_FROM:-}" ] || [ -n "${VMRUN_TO:-}" ]; then
            export START_AT=${VMRUN_FROM:-1} RETRY_DAMAGED=0
            [ -n "${VMRUN_TO:-}" ] && export TOTAL=$VMRUN_TO
            VMRUN_NO_INTERACTIVE=1
        fi
        guarded "$outdir/" "${VMRUN_TIMEOUT:-7200}" "$REPO/scripts/sunit-sweep.sh" "$VM" "$image" "$outdir" > "$outdir/driver.log" 2>&1 < /dev/null
        rc=$?
    else
        # cog-sweep.sh runs <dir>/pharo <image>: give it a directory with this VM.
        cogdir=$outdir/cogdir; rm -rf "$cogdir"; mkdir -p "$cogdir"
        printf '#!/bin/bash\nexec "%s" --headless %s %s "$@"\n' "$VM" "$(cog_mem)" "$COG_FRAMES" > "$cogdir/pharo"; chmod +x "$cogdir/pharo"
        cp "$image" "$cogdir/Pharo.image" && cp "${image%.image}.changes" "$cogdir/Pharo.changes" && cp "$(dirname "$image")"/*.sources "$cogdir/"
        # The same curated class order as the sweep of our VM; the runner adds
        # every other TestCase subclass of the image after it.
        list=${VMRUN_CLASS_LIST:-$REPO/scripts/pharo-headless-test/test_classes.txt}
        [ -f "$list" ] || { say "no class list $list (set VMRUN_CLASS_LIST)"; exit 2; }
        export COG_ARCH=native STEP=${STEP:-100} PER_BATCH_TIMEOUT=${PER_BATCH_TIMEOUT:-2400}
        guarded "$outdir/" "${VMRUN_TIMEOUT:-7200}" "$REPO/scripts/cog-sweep.sh" "$list" "$outdir" "$cogdir" > "$outdir/driver.log" 2>&1 < /dev/null
        rc=$?
    fi
    tail -n 14 "$outdir/sweep.log" 2>/dev/null
    echo "sweep rc=$rc"
    # The interactive pass: the classes of scripts/interactive-classes.txt have
    # tests that skip in the batches, because the session there is not
    # interactive.  For our VM the pass needs no window.  For stock Cog it
    # opens a window, so it runs only on request (vmrun.sh interactive).
    if [ "$KIND" = ours ] && [ "${VMRUN_NO_INTERACTIVE:-0}" != 1 ]; then
        export SUNIT_PREFIX=$outdir/interactive
        rm -f "${SUNIT_PREFIX}"_*
        grep -v '^#' "$REPO/scripts/interactive-classes.txt" | awk 'NF { print $1 }' > "${SUNIT_PREFIX}_class_names.txt"
        fresh_rundir "$image"
        cd "$RUNDIR" || exit 2
        export PHARO_INTERACTIVE=1 PHARO_EVAL_NO_SUNIT_MARKER=1
        guarded "$RUNDIR/" 900 "$VM" "$RUNDIR/rt.image" eval "'$REPO/scripts/pharo-headless-test/run_sunit.st' asFileReference fileIn" > "$outdir/interactive.log" 2>&1 < /dev/null
        irc=$?
        echo "=== INTERACTIVE PASS (rc=$irc) ==="
        if [ -f "${SUNIT_PREFIX}_test_results.txt" ]; then
            awk '/^Total:/ && NF > 2 { t += $2; for (i = 3; i <= NF; i++) { split($i, kv, ":"); n[kv[1]] += kv[2] } }
                 END { printf "  tests   %d\n  PASS    %d\n  FAIL    %d\n  ERROR   %d\n  SKIP    %d\n  TIMEOUT %d\n  XFAIL   %d\n", t, n["P"], n["F"], n["E"], n["S"], n["T"], n["X"] }' "${SUNIT_PREFIX}_test_results.txt"
            grep -E '^  (FAIL|ERROR|SKIP|TIMEOUT|XFAIL)' "${SUNIT_PREFIX}_test_results.txt" | cut -c1-200
        else
            echo "  no results (log: $outdir/interactive.log)"
        fi
    fi
    ;;

interactive)
    # The classes of scripts/interactive-classes.txt in an interactive session,
    # with the sweep runner.  Our VM: PHARO_INTERACTIVE=1.  Cog: a start without
    # --headless, so a real window opens on the desktop for the run.
    name=${1:?usage: vmrun.sh interactive <vm> <image> <outdir>}; image=${2:?image}; outdir=${3:?outdir}
    resolve_vm "$name"
    acquire_lock "interactive $name $(basename "$outdir")"
    mkdir -p "$outdir"; outdir=$(cd "$outdir" && pwd)
    export SUNIT_PREFIX=$outdir/state
    rm -f "${SUNIT_PREFIX}"_*
    grep -v '^#' "$REPO/scripts/interactive-classes.txt" | awk 'NF { print $1 }' > "${SUNIT_PREFIX}_class_names.txt"
    fresh_rundir "$image"
    cd "$RUNDIR" || exit 2
    expr="'$REPO/scripts/pharo-headless-test/run_sunit.st' asFileReference fileIn"
    if [ "$KIND" = ours ]; then
        ours_env
        export PHARO_INTERACTIVE=1 PHARO_EVAL_NO_SUNIT_MARKER=1
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" "$RUNDIR/rt.image" eval "$expr" > "$outdir/run.log" 2>&1 < /dev/null
    else
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" "$(cog_mem)" "$COG_FRAMES" "$RUNDIR/rt.image" eval "$expr" > "$outdir/run.log" 2>&1 < /dev/null
    fi
    rc=$?
    if [ -f "${SUNIT_PREFIX}_test_results.txt" ]; then
        cp "${SUNIT_PREFIX}_test_results.txt" "$outdir/all_results.txt"
        grep -E '^(=== [A-Z]|Total:|  (FAIL|ERROR|SKIP|TIMEOUT|XFAIL))' "$outdir/all_results.txt" | cut -c1-220
    else
        echo "NO-RESULTS rc=$rc (log: $outdir/run.log)"; tail -5 "$outdir/run.log"
    fi
    echo "interactive rc=$rc"
    ;;

check)
    # The memory check, for a script that vmrun.sh started and that starts
    # more than one VM (run-one-package.sh).  No lock: the caller has it.
    check_memory_before_start
    ;;

pkgs)
    # Rows <first>..<last> of scripts/pkg-jit-test/packages-200.tsv, one package
    # after the other.  For each package: stock Cog loads the package from
    # GitHub and saves the image, then stock Cog and our VM run the tests of
    # the package with run_sunit.st (scripts/pkg-jit-test/run-one-package.sh).
    # The lock is free between two packages, so a short call (a test, an eval)
    # gets its turn.  A package with a final row in <outdir>/summary.tsv is
    # not run again (--redo runs it again).
    outdir=${1:?usage: vmrun.sh pkgs <outdir> <first-row> [last-row] [ours-vm] [cog-vm]}; first=${2:?first row}; last=${3:-$first}
    resolve_vm "${5:-cog}"; COG=$VM
    resolve_vm "${4:-ours}"; OURS=$VM
    export COG OURS
    export VMRUN_CHECK="$REPO/scripts/vmrun.sh"
    [ "${VMRUN_REDO:-0}" = 1 ] && export PKG_REDO=1
    [ "${VMRUN_REUSE:-0}" = 1 ] && export PKG_REUSE_LOAD=1
    mkdir -p "$outdir"; outdir=$(cd "$outdir" && pwd)
    ours_env
    row=$first; tries=0
    while [ "$row" -le "$last" ]; do
        label=$(tail -n +2 "$REPO/scripts/pkg-jit-test/packages-200.tsv" | sed -n "${row}p" | cut -f1)
        [ -n "$label" ] || { echo "row $row: no such row"; break; }
        acquire_lock "pkgs $(basename "$outdir") row $row $label"
        guarded "$outdir/" "${VMRUN_TIMEOUT:-7200}" "$REPO/scripts/pkg-jit-test/run-one-package.sh" "$outdir" "$row" >> "$outdir/driver.log" 2>&1 < /dev/null
        rc=$?
        release_lock
        sleep 3                     # a call that waits for the lock looks each 2 seconds
        if [ "$rc" -eq 4 ] && [ "$tries" -lt 5 ]; then
            # The memory check refused a VM start: the same row again, later.
            tries=$((tries + 1)); echo "row $row: memory check refused, try $tries"; sleep 120; continue
        fi
        tries=0
        [ "$rc" -ne 0 ] && echo "row $row: rc=$rc (log: $outdir/driver.log)"
        awk -F'\t' -v l="$label" '$1 == l { r = $0 } END { if (r != "") print r; else print l "\tNO-ROW" }' "$outdir/summary.tsv" 2>/dev/null | cut -c1-230
        row=$((row + 1))
    done
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
        guarded "$RUNDIR/" "${VMRUN_TIMEOUT:-900}" "$VM" --headless "$(cog_mem)" "$COG_FRAMES" "$RUNDIR/rt.image" "$@" < /dev/null
    fi
    echo "run rc=$? (directory: $RUNDIR)"
    ;;

cogbuild)
    # Build the Cog tree that has the changes of docs/upstream
    # ($PHARO_WORK/cogbuild-all, source $PHARO_WORK/wt/cog-all), under the lock,
    # and copy its core library, its socket plugin and its file plugin
    # (NewFilePlugin) into the app of the VM name cogp.  The other libraries
    # of that app are the stock ones: a local Cog build has no freetype, cairo
    # and libgit2.
    acquire_lock "cogbuild"
    check_memory_before_start
    [ -d "$WORK/cogbuild-all" ] || { say "no build tree $WORK/cogbuild-all"; exit 2; }
    cmake --build "$WORK/cogbuild-all" -j4 > "$WORK/cogbuild-all/build-last.log" 2>&1
    rc=$?
    grep -E ' error |Error [0-9]|FAILED' "$WORK/cogbuild-all/build-last.log" | head -20
    src=$WORK/cogbuild-all/build/vm/Debug/Pharo.app/Contents/MacOS/Plugins
    dst=$WORK/cogp-app/Pharo.app/Contents/MacOS/Plugins
    if [ "$rc" -eq 0 ]; then
        [ -f "$dst/libNewFilePlugin.dylib.stock" ] || cp "$dst/libNewFilePlugin.dylib" "$dst/libNewFilePlugin.dylib.stock"
        cp "$src/libPharoVMCore.dylib" "$dst/" && cp "$src/libSocketPlugin.dylib" "$dst/" && cp "$src/libNewFilePlugin.dylib" "$dst/" || exit 2
        ls -la "$dst/libPharoVMCore.dylib" "$dst/libSocketPlugin.dylib" "$dst/libNewFilePlugin.dylib"
    fi
    echo "cogbuild rc=$rc (log: $WORK/cogbuild-all/build-last.log)"
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
    sed -n '3,52p' "${BASH_SOURCE[0]}"
    exit 2
    ;;
esac
