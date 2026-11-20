#!/usr/bin/env python3
"""Compare a Pharo 15 sweep with the list of the 220 Pharo 14 tests.

usage: p15cmp.py <sweep-dir> [<pharo14-sweep-detail.gz>]
Prints the totals of the sweep, the state on Pharo 15 of each test of the
list (base-* and found rows), and the tests that do not pass on Pharo 15 and
are not in the list.  Writes <sweep-dir>/cmp.tsv.
"""
import collections, gzip, os, sys

sweep = sys.argv[1]
LIST = '/Users/wohl/src/iospharo/docs/results/nonpassing-tests-2026-11-18.tsv'

def load(path):
    res = collections.defaultdict(list)
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 4:
                res[(p[1], p[2].split('(')[0])].append(p[3])
    return res

d = load(os.path.join(sweep, 'all_detail.txt'))
tot = collections.Counter(s for v in d.values() for s in v)
print('sweep', sweep, 'tests', sum(tot.values()), dict(tot))

rows = [l.rstrip('\n').split('\t') for l in open(LIST)][1:]
listed = {}
for r in rows:
    if r[0].startswith('base') or r[0] == 'found':
        listed[(r[1], r[2])] = r

def summ(v):
    if not v:
        return 'not run'
    c = collections.Counter(v)
    return ', '.join(f'{k} {n}x' if n > 1 else k for k, n in sorted(c.items()))

out = ['part\tclass\tselector\tgroup\tstate_p14\tbefore_p14\tp15']
by = collections.Counter()
for k, r in listed.items():
    s = summ(d.get(k))
    before = r[10] if len(r) > 10 else ''
    out.append('\t'.join([r[0], r[1], r[2], r[3], r[5], before, s]))
    key = 'PASS' if s.startswith('PASS') and ',' not in s else ('not run' if s == 'not run' else 'not pass: ' + s.split(' ')[0])
    by[(r[0], key)] += 1
open(os.path.join(sweep, 'cmp.tsv'), 'w').write('\n'.join(out) + '\n')
for k, v in sorted(by.items()):
    print('listed', k, v)

new = sorted((k, v) for k, v in d.items() if k not in listed and any(s != 'PASS' for s in v))
print('not passing on Pharo 15 and not in the list:', len(new))
for (c, s), v in new:
    print('  ', c, s, summ(v))
