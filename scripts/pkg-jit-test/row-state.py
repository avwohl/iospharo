#!/usr/bin/env python3
"""row-state.py -- the state of each row of a package run.

    python3 scripts/pkg-jit-test/row-state.py <outdir> [first-row [last-row]]

Reads <outdir>/summary.tsv (the last line of each label), the manifest
packages-200.tsv, and the files not-applicable/<label>.txt (tests that cannot
apply on this machine, each with its proof).

A row is CLEAN when the two VMs ran the full class list and each test passes
on stock Cog and on our VM.  A row is CLEAN-NA when each test that does not
pass is in the not-applicable file of the row, on the two VMs, as a skip.
Each other row is listed with its counts.
"""
import os, re, sys

here = os.path.dirname(os.path.abspath(__file__))

def not_applicable(label):
    path = os.path.join(here, 'not-applicable', label + '.txt')
    if not os.path.exists(path):
        return []
    return [l.strip() for l in open(path, encoding='utf-8')
            if l.strip() and not l.startswith('#')]

def non_pass(path):
    """(test, status) for each test of a status file that is not PASS."""
    out = []
    if not os.path.exists(path):
        return None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line:
            continue
        test, _, status = line.rpartition(' ')
        if status != 'PASS':
            out.append((test, status))
    return out

def covered(test, patterns):
    cls = test.split('>>')[0]
    for p in patterns:
        if p == test or p == cls + '>>*':
            return True
        # a parametrized test has a suffix after the selector
        if test.startswith(p) and not test[len(p):len(p) + 1].isalnum():
            return True
    return False

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    last = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 6
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(here, 'packages-200.tsv'), encoding='utf-8')][1:]
    order = {r[0]: i + 1 for i, r in enumerate(rows)}
    seen = {}
    for line in open(os.path.join(outdir, 'summary.tsv'), encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if f and f[0] in order:
            seen[f[0]] = f
    counts = {'CLEAN': 0, 'CLEAN-NA': 0, 'NOT-CLEAN': 0, 'NO-RESULT': 0}
    na_tests = 0
    lines = []
    # A row that runs again has no line in summary.tsv until the run ends.
    # Such a row is listed, so that it does not look clean by its absence.
    highest = max([order[l] for l in seen] or [0])
    missing = [(i + 1, r[0]) for i, r in enumerate(rows)
               if r[0] not in seen and first <= i + 1 <= min(last, highest)]
    for n, label in missing:
        counts['NO-RESULT'] += 1
        lines.append((n, '%3d %-46s NO-RESULT        no line in summary.tsv (a run of the row is in work, or the row did not run)' % (n, label)))
    for label, f in sorted(seen.items(), key=lambda kv: order[kv[0]]):
        n = order[label]
        if n < first or n > last:
            continue
        state = f[1]
        cog = f[3] if len(f) > 3 else '-'
        ours = f[4] if len(f) > 4 else '-'
        zero = ' F=0 E=0 S=0 T=0 X=0'
        if state == 'ok' and zero in cog and zero in ours and cog == ours:
            counts['CLEAN'] += 1
            continue
        patterns = not_applicable(label)
        kind = 'NOT-CLEAN'
        if state == 'ok' and patterns:
            bad_cog = non_pass(os.path.join(outdir, 'res', label + '.cog.status'))
            bad_ours = non_pass(os.path.join(outdir, 'res', label + '.ours.status'))
            if bad_cog is not None and bad_ours is not None \
                    and all(s == 'SKIP' and covered(t, patterns) for t, s in bad_cog + bad_ours):
                kind = 'CLEAN-NA'
                na_tests += len(bad_cog)
        counts[kind] += 1
        note = ''
        if kind == 'NOT-CLEAN' and state == 'ok' and patterns:
            # Part of the tests that do not pass cannot apply: say how many
            # of the others are open on each VM.
            bad_cog = non_pass(os.path.join(outdir, 'res', label + '.cog.status')) or []
            bad_ours = non_pass(os.path.join(outdir, 'res', label + '.ours.status')) or []
            open_cog = [t for t, s in bad_cog if not (s == 'SKIP' and covered(t, patterns))]
            open_ours = [t for t, s in bad_ours if not (s == 'SKIP' and covered(t, patterns))]
            note = '   open: cog %d, ours %d (the others cannot apply)' % (len(open_cog), len(open_ours))
        if kind == 'CLEAN-NA':
            lines.append((n, '%3d %-46s CLEAN-NA   %d tests cannot apply (not-applicable/%s.txt)' % (n, label, len(bad_cog), label)))
        else:
            lines.append((n, '%3d %-46s %-16s cog[%s] ours[%s]%s' % (n, label, state, cog.replace('tests=', ''), ours.replace('tests=', ''), note)))
    print('\n'.join(text for _, text in sorted(lines)))
    total = counts['CLEAN'] + counts['CLEAN-NA'] + counts['NOT-CLEAN']
    print('rows with a result: %d   clean: %d   clean, with tests that cannot apply: %d (%d tests)   not clean: %d   no result now: %d'
          % (total, counts['CLEAN'], counts['CLEAN-NA'], na_tests, counts['NOT-CLEAN'], counts['NO-RESULT']))

if __name__ == '__main__':
    main()
