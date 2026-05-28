#!/usr/bin/env python3
"""Δcog diff for the JIT package campaign.

Our VM's runner records per-test results in the DETAIL file (tab format):
    <run>\t<Class>\t<selector>\t<STATUS>      STATUS in PASS/FAIL/ERROR/SKIP/TIMEOUT
The stock-Cog runner records them in the RESULTS file (=== Class === format):
    === Class ===
      RUN: selector
      FAIL: selector [msg]      (ERROR/SKIP/TIMEOUT, or nothing => pass)

This tool parses either format into dict[(class, selector)] = status and reports
tests that PASS on cog but regress on ours (the JIT/VM-bug candidates), plus tests
that fail on BOTH (pre-existing image/env issues, not our bug).

usage: diff_jit_pkg.py <cog-results.txt> <ours-detail.txt>
"""
import re
import sys
from pathlib import Path

CLASS_RE = re.compile(r'^=== (\w+) ===$')
RUN_RE = re.compile(r'^  RUN: (\S+)$')
RESULT_RE = re.compile(r'^  (FAIL|ERROR|SKIP|TIMEOUT): (\S+)')
TAB_RE = re.compile(r'^\d+\t([^\t]+)\t([^\t]+)\t(PASS|FAIL|ERROR|SKIP|TIMEOUT)\s*$')


def parse(path: Path) -> dict:
    """Auto-detect tab format (ours) vs === format (cog)."""
    results = {}
    text = path.read_text(errors='replace')
    # Try tab format (our VM detail) first: match each line independently.
    for line in text.splitlines():
        m = TAB_RE.match(line)
        if m:
            results[(m.group(1), m.group(2))] = m.group(3).lower()
    if results:
        return results
    # === Class === format
    cur, pending = None, None
    for line in text.splitlines():
        m = CLASS_RE.match(line)
        if m:
            if cur and pending:
                results[(cur, pending)] = 'pass'
            cur, pending = m.group(1), None
            continue
        m = RUN_RE.match(line)
        if m and cur:
            if pending:
                results[(cur, pending)] = 'pass'
            pending = m.group(1)
            continue
        m = RESULT_RE.match(line)
        if m and cur and pending and m.group(2) == pending:
            results[(cur, pending)] = m.group(1).lower()
            pending = None
    if cur and pending:
        results[(cur, pending)] = 'pass'
    return results


def counts(r):
    c = {}
    for s in r.values():
        c[s] = c.get(s, 0) + 1
    return c


def main():
    cog = parse(Path(sys.argv[1]))
    ours = parse(Path(sys.argv[2]))
    print(f'cog : total={len(cog)}  ' + '  '.join(f'{k}={v}' for k, v in sorted(counts(cog).items())))
    print(f'ours: total={len(ours)}  ' + '  '.join(f'{k}={v}' for k, v in sorted(counts(ours).items())))

    jit_bugs, both_fail, only_ours = [], [], []
    for key, ostatus in sorted(ours.items()):
        if ostatus == 'pass':
            continue
        cstatus = cog.get(key)
        if cstatus is None:
            only_ours.append((key, ostatus))
        elif cstatus == 'pass':
            jit_bugs.append((key, ostatus))
        else:
            both_fail.append((key, ostatus, cstatus))

    print(f'\n*** JIT/VM-BUG CANDIDATES (pass on cog, {len(jit_bugs)} regress on ours) ***')
    for (cls, sel), st in jit_bugs:
        print(f'  {st:8} {cls}>>{sel}')
    if both_fail:
        print(f'\n--- fail on BOTH (pre-existing, not our bug): {len(both_fail)} ---')
        for (cls, sel), ost, cst in both_fail:
            print(f'  ours={ost:7} cog={cst:7} {cls}>>{sel}')
    if only_ours:
        print(f'\n--- non-pass on ours, not present in cog run: {len(only_ours)} ---')
        for (cls, sel), st in only_ours:
            print(f'  {st:8} {cls}>>{sel}')


if __name__ == '__main__':
    main()
