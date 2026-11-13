#!/usr/bin/env bash
# run-cogdiff.sh -- run each cogdiff script on stock Cog and on our VM, and
# compare the answers.
#
#   PHARO=/c/h/stock/pharo BASE_IMAGE=/c/h/stock/Pharo.image \
#   CUSTOM_VM=$PWD/build-win/test_load_image.exe OUT=/c/h/cogdiff \
#     scripts/cogdiff/run-cogdiff.sh [script.st ...]
#
# MODES is a list of our-VM configurations.  Each is a name, or name:VAR=1.
# Default: "jit nojit:PHARO_NO_JIT=1 nomcf:PHARO_NO_MCF=1".
# Each run has a new directory with a new copy of the image, because the
# scripts compile classes, and because our eval mode writes startup.st into
# the current directory.
# The answer of a run is the eval result, or the file that the script writes
# (primdiff, primdiff2, edgediff).  $OUT/summary.txt has one line per script
# and mode: SAME, or DIFF with the count of different lines.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PHARO="${PHARO:?set PHARO to the stock pharo launcher}"
BASE_IMAGE="${BASE_IMAGE:?set BASE_IMAGE to a pristine Pharo.image}"
CUSTOM_VM="${CUSTOM_VM:?set CUSTOM_VM to test_load_image}"
OUT="${OUT:?set OUT to a scratch directory}"
MODES="${MODES:-jit nojit:PHARO_NO_JIT=1 nomcf:PHARO_NO_MCF=1}"
RUN_TIMEOUT="${RUN_TIMEOUT:-1200}"
base_dir="$(dirname "$BASE_IMAGE")"

# A path that both bash and a native Windows program accept.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

# CR LF and a CR alone both become LF.  A Smalltalk cr in an answer is CR LF
# on the stdout of the stock Windows VM and a CR alone on our VM.
nl_norm() { sed -e 's/\r$//' | tr '\r' '\n'; }

if [ $# -gt 0 ]; then scripts=("$@"); else scripts=("$HERE"/*.st); fi
mkdir -p "$OUT"
: > "$OUT/summary.txt"

# one_run <script> <mode-name> <env-assignment-or-empty> : leaves $d/answer.txt
one_run() {
    local st="$1" name="$2" assign="$3"
    local s d text
    s="$(basename "$st" .st)"
    d="$OUT/$s/$name"
    rm -rf "$d"; mkdir -p "$d/fdir"
    cp "$BASE_IMAGE" "$d/C.image"
    cp "${BASE_IMAGE%.image}.changes" "$d/C.changes"
    cp "$base_dir"/*.sources "$d/" 2>/dev/null
    # The output file and directory globals become literal paths of this run.
    # primdiff: Cog crashes on ByteString compareWith: 1, so no VM gets the
    # two classes.
    text="$(tr -d '\r' < "$st" | sed \
        -e "s|(Smalltalk at: #PRIMDIFFOUT ifAbsent: \[ '/tmp/primdiff-out.txt' \])|'$(native "$d")/fileout.txt'|" \
        -e "s|(Smalltalk at: #EDGEOUT ifAbsent: \['/tmp/edgediff-out.txt'\])|'$(native "$d")/fileout.txt'|" \
        -e "s|^d := (Smalltalk at: #FDIR) asFileReference|d := '$(native "$d")/fdir' asFileReference|")"
    # edgediff: with EDGE_NO_WAIT=1 the last expression, a wait on a new
    # Semaphore, is left out.  On Windows the stock VM and our VM both wait
    # for ever there, so no run of the script ends.
    if [ "$s" = edgediff ] && [ "${EDGE_NO_WAIT:-0}" = 1 ]; then
        text="$(printf '%s' "$text" | sed -e "s/ '(Semaphore new) wait; yourself'\$//")"
    fi
    if [ "$s" = primdiff ]; then
        text="$(printf '%s' "$text" | sed -e 's/ ByteString\. / /' -e 's/ ByteSymbol\. / /')"
    fi
    (
        cd "$d" || exit 1
        if [ "$name" = cog ]; then
            timeout "$RUN_TIMEOUT" "$PHARO" C.image eval "$text" > stdout.txt 2> stderr.txt
        else
            # PHARO_CANNOT_RETURN_BURST: our VM stops a process after 64
            # handled BlockCannotReturn signals (docs/vm-compat-bugs.md,
            # cannotReturnStormGuard, open).  nlrhome.st makes thousands.
            env $assign PHARO_EVAL_NO_SUNIT_MARKER=1 PHARO_MAX_STEPS=2000000000000 \
                PHARO_CANNOT_RETURN_BURST=1000000000 \
                timeout "$RUN_TIMEOUT" "$CUSTOM_VM" C.image eval "$text" > stdout.txt 2> stderr.txt
        fi
        echo $? > exit.txt
    )
    if [ -f "$d/fileout.txt" ]; then
        nl_norm < "$d/fileout.txt" > "$d/answer.txt"
    elif [ "$name" = cog ]; then
        nl_norm < "$d/stdout.txt" > "$d/answer.txt"
    else
        # Our VM prints the answer on stderr after the marker EVAL-RESULT= at
        # the start of a line.  The echo of the image stdout (each line has
        # the prefix [IMAGE]) or the JIT statistics follow the answer.
        nl_norm < "$d/stderr.txt" \
            | perl -0777 -ne 'if (/^EVAL-RESULT=(.*?)(?=\[IMAGE\] |\n+=== JIT Stats ===|\z)/ms) { $a = $1; $a =~ s/\n+\z//; print $a, "\n" }' > "$d/answer.txt"
    fi
    rm -f "$d/C.image" "$d/C.changes" "$d"/*.sources
}

for st in "${scripts[@]}"; do
    s="$(basename "$st" .st)"
    one_run "$st" cog ""
    cogrc="$(cat "$OUT/$s/cog/exit.txt")"
    for m in $MODES; do
        name="${m%%:*}"; assign=""
        case "$m" in *:*) assign="${m#*:}";; esac
        one_run "$st" "$name" "$assign"
        rc="$(cat "$OUT/$s/$name/exit.txt")"
        # an answer in one line: break it at the scenario separators to count
        for f in "$OUT/$s/cog/answer.txt" "$OUT/$s/$name/answer.txt"; do
            # the directory of the run is in some answers (filediff)
            sed -e 's/\. #/.\n#/g' -e 's/ -> /->/g' \
                -e "s#[\\\\/]$s[\\\\/]\\(cog\\|$name\\)[\\\\/]#/$s/RUN/#g" "$f" > "$f.lines"
        done
        n="$(diff "$OUT/$s/cog/answer.txt.lines" "$OUT/$s/$name/answer.txt.lines" | grep -c '^[<>]')"
        lines="$(wc -l < "$OUT/$s/cog/answer.txt.lines" | tr -d ' ')"
        if [ "$n" = 0 ] && [ -s "$OUT/$s/cog/answer.txt" ]; then v=SAME; else v="DIFF $n"; fi
        printf '%-16s %-8s %-10s cog_exit=%s our_exit=%s cog_lines=%s\n' "$s" "$name" "$v" "$cogrc" "$rc" "$lines" \
            | tee -a "$OUT/summary.txt"
    done
done
