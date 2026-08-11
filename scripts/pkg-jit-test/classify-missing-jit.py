#!/usr/bin/env python3
"""Explain every sweep row where stock Cog produced a RESULT and our VM did not.

The 200-package sweep records such a row as `jit_RESULT = -`, which says nothing
about WHY.  The 2026-08-11 arm sweep left 15 of them unexamined with the note
"some are probably the 900 s JIT timeout, but that has not been checked" — this
checks.  Each is one of:

    timeout      tests were running and the per-package budget ran out; the
                 last selector seen names where it was.  Not a correctness
                 result — a speed one.
    oom-killed   SIGKILL: the kernel's OOM killer took this process out, which
                 usually says something about a NEIGHBOUR (one runaway
                 stock-Cog run reached 30 GB on a 32 GB box during the
                 2026-08-11 sweep and three unrelated packages "crashed").
                 Re-run standalone before believing anything about it.
    crash        the VM died (signal, assertion, VM-level abort).
    no-classes   the runner selected 0 test classes, so there was nothing to
                 run; the package's test prefix does not match what it loaded.
    eval-lost    the startup script never ran (the CWD startup.st trap).
    unknown      none of the above matched; read the log.

Usage:  classify-missing-jit.py <sweep-out-dir> [--all]
        <sweep-out-dir> holds summary.tsv and <label>_{cog,jit}.log, or the
        per-worker w*/ subdirectories a parallel sweep produces.
        --all also classifies rows where NEITHER arm produced a RESULT.
"""
import re
import sys
from pathlib import Path

RESULT_RE = re.compile(r"^RESULT ", re.M)
CLASSES_RE = re.compile(r"^CLASSES (\d+)", re.M)
RUNNING_RE = re.compile(r"^(?:FAIL|ERR|TIMEOUT) (\S+)", re.M)
CRASH_RE = re.compile(
    r"Segmentation fault|SIGSEGV|SIGBUS|SIGABRT|Assertion .* failed"
    r"|\*\*\* stack smashing|terminate called|VM ABORT|\[FATAL\]", re.I)
# A test's own failure text says things like "[Assertion failed]" — that is a
# reported result, not a dead VM.  Only lines the RUNNER did not produce count.
RUNNER_LINE_RE = re.compile(r"^(FAIL|ERR|TIMEOUT|RESULT|CLASSES|PREFIXES)\b")


def crash_line(text):
    for line in text.splitlines():
        stripped = line.lstrip("\x00 ")
        if RUNNER_LINE_RE.match(stripped):
            continue
        if CRASH_RE.search(stripped):
            return stripped.strip()
    return None


def read(path):
    try:
        return path.read_bytes().decode("utf-8", "replace")
    except OSError:
        return ""


def find_log(dirs, label):
    """A parallel sweep merges summary.tsv at the top but leaves each package's
    logs in the worker directory that produced it, so look in every candidate."""
    for d in dirs:
        p = d / f"{label}_jit.log"
        if p.exists():
            return p
    return None


def classify(log_path):
    """-> (verdict, detail)"""
    if log_path is None or not log_path.exists():
        return "unknown", "no jit log on disk"
    # The recorded exit status is more reliable than anything in the log: a
    # SIGSEGV printed by our own handler and a SIGSEGV caused by the kernel
    # OOM-killing a NEIGHBOUR look identical in the text.
    exit_path = log_path.with_name(log_path.name.replace(".log", ".exit"))
    if exit_path.exists():
        code = read(exit_path).strip()
        if code == "137":
            return "oom-killed", "SIGKILL (exit 137) — says nothing about this package"
        if code == "124":
            return "timeout", "budget exhausted (exit 124)"
    text = read(log_path)
    if not text.strip():
        return "unknown", "jit log is empty"
    line = crash_line(text)
    if line:
        return "crash", line[:160]
    m = CLASSES_RE.search(text)
    if m and int(m.group(1)) == 0:
        return "no-classes", "runner selected 0 test classes"
    if not m:
        # The runner prints PREFIXES then CLASSES before running anything.
        if "PREFIXES" not in text:
            return "eval-lost", "runner never started (no PREFIXES line)"
        return "unknown", "PREFIXES seen but no CLASSES line"
    seen = RUNNING_RE.findall(text)
    where = f"last non-pass seen: {seen[-1]}" if seen else "no non-pass reported"
    return "timeout", f"{m.group(1)} classes selected, no RESULT; {where}"


def summaries(root: Path):
    direct = root / "summary.tsv"
    if direct.exists():
        yield root, direct
    for w in sorted(root.glob("w*/summary.tsv")):
        yield w.parent, w


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    root = Path(argv[0])
    want_all = "--all" in argv[1:]
    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 2

    log_dirs = [root] + sorted(p for p in root.glob("w*") if p.is_dir())
    rows, seen_labels = [], set()
    for out_dir, summary in summaries(root):
        for line in read(summary).splitlines()[1:]:
            parts = line.split("\t")
            if len(parts) < 4 or not parts[0]:
                continue
            label, load, cog, jit = parts[0], parts[1], parts[2], parts[3]
            if label in seen_labels:
                continue
            if load != "ok":
                continue
            cog_ran = cog.startswith("RESULT")
            jit_ran = "RESULT" in jit
            if jit_ran or (not cog_ran and not want_all):
                continue
            seen_labels.add(label)
            verdict, detail = classify(find_log([out_dir] + log_dirs, label))
            rows.append((verdict, label, "cog ran" if cog_ran else "cog did not",
                         detail))

    if not rows:
        print("no rows where our VM failed to produce a RESULT")
        return 0

    order = {"crash": 0, "unknown": 1, "eval-lost": 2, "oom-killed": 3,
             "timeout": 4, "no-classes": 5}
    rows.sort(key=lambda r: (order.get(r[0], 9), r[1]))
    width = max(len(r[1]) for r in rows)
    for verdict, label, cog_state, detail in rows:
        print(f"  {verdict:<11} {label:<{width}}  {cog_state:<12} {detail}")
    print()
    counts = {}
    for verdict, *_ in rows:
        counts[verdict] = counts.get(verdict, 0) + 1
    print("  " + "  ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    print(f"  total={len(rows)}")
    # A crash is the only verdict here that is a VM defect.
    return 1 if counts.get("crash") or counts.get("unknown") else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
