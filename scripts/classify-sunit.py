#!/usr/bin/env python3
"""Join SUnit result files and count test-level deltas.

Each result file has the shape:
  === ClassName ===
    RUN: selector
    FAIL: selector [msg]    (or ERROR/SKIP/TIMEOUT/no-line-if-pass)
    ...
  Total: N P:P F:F E:E S:S [T:T]

Status for a (class, selector) pair:
  pass     — had RUN line, no FAIL/ERROR/SKIP/TIMEOUT line after
  fail     — FAIL line matched
  error    — ERROR line matched
  skip     — SKIP line matched
  timeout  — TIMEOUT line matched

We parse into dict[(class, selector)] = status and compute deltas.
"""

import re
import sys
from pathlib import Path

CLASS_RE = re.compile(r'^=== (\w+) ===$')
RUN_RE = re.compile(r'^  RUN: (\S+)$')
RESULT_RE = re.compile(r'^  (FAIL|ERROR|SKIP|TIMEOUT): (\S+)')


def parse(path: Path) -> dict[tuple[str, str], str]:
    results: dict[tuple[str, str], str] = {}
    cur_class = None
    pending: str | None = None
    with path.open() as f:
        for line in f:
            m = CLASS_RE.match(line.rstrip())
            if m:
                if cur_class and pending:
                    results[(cur_class, pending)] = 'pass'
                cur_class = m.group(1)
                pending = None
                continue
            m = RUN_RE.match(line.rstrip())
            if m and cur_class:
                if pending:
                    results[(cur_class, pending)] = 'pass'
                pending = m.group(1)
                continue
            m = RESULT_RE.match(line)
            if m and cur_class and pending:
                status = m.group(1).lower()
                sel = m.group(2)
                if sel == pending:
                    results[(cur_class, pending)] = status
                    pending = None
        if cur_class and pending:
            results[(cur_class, pending)] = 'pass'
    return results


def summarize(tag: str, r: dict):
    counts = {}
    for status in r.values():
        counts[status] = counts.get(status, 0) + 1
    total = sum(counts.values())
    print(f'{tag:15}  total={total}  ' +
          '  '.join(f'{k}={v}' for k, v in sorted(counts.items())))


def delta_vs_cog(cog: dict, other: dict, other_tag: str):
    """Tests that pass on cog but not on other."""
    regressions_by_status = {}
    regressions = []
    missing_from_other = 0
    for key, cog_status in cog.items():
        if cog_status != 'pass':
            continue
        other_status = other.get(key)
        if other_status is None:
            missing_from_other += 1
            continue
        if other_status != 'pass':
            regressions.append((key, other_status))
            regressions_by_status[other_status] = regressions_by_status.get(other_status, 0) + 1
    print()
    print(f'Δcog for {other_tag}:  '
          f'{len(regressions)} regression(s) of cog-pass'
          f'  (+{missing_from_other} cog-tests not run on {other_tag})')
    for status in sorted(regressions_by_status):
        print(f'  {status:10}  {regressions_by_status[status]}')
    return regressions


def main():
    if len(sys.argv) < 3:
        print('usage: classify-sunit.py cog.txt other.txt [other2.txt ...]',
              file=sys.stderr)
        sys.exit(2)
    cog_path = Path(sys.argv[1])
    cog = parse(cog_path)
    summarize(cog_path.stem, cog)
    for other_arg in sys.argv[2:]:
        other_path = Path(other_arg)
        other = parse(other_path)
        summarize(other_path.stem, other)
        regs = delta_vs_cog(cog, other, other_path.stem)
        out_path = other_path.with_suffix('.delta-vs-cog.txt')
        with out_path.open('w') as f:
            f.write(f'# {len(regs)} tests pass on cog but not on {other_path.stem}\n')
            f.write(f'# columns: status\tclass\tselector\n')
            for (cls, sel), status in sorted(regs):
                f.write(f'{status}\t{cls}\t{sel}\n')
        print(f'  wrote {out_path}')


if __name__ == '__main__':
    main()
