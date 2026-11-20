#!/usr/bin/env python3
"""Compare Pharo 14 (build 791) and Pharo 15 (build 141) sources for
(1) every test of the non-passing list (base-* and found rows), and
(2) every method that the fix entries 20-35 replace (fix value: #Class value: #sel).
Writes p14p15-tests.tsv and p14p15-fixes.tsv next to this script."""
import collections, glob, os, re, sys
sys.path.insert(0, '/Users/wohl/src/iospharo/scripts/pharo-bugs-site')
HERE = os.environ.get('OUT_DIR', '.')

src = open('/Users/wohl/src/iospharo/scripts/pharo-bugs-site/gen.py').read()
ns = {}
ns.update({'re': re, 'os': os}); exec(src[src.index('def chunks(text):'):src.index("print('reading Pharo 14 sources'")], ns)
methods = ns['methods']
P14 = os.path.expanduser('~/pharo-work-save/2026-11-17/images/Pharo14.0-64bit-42a90c7.sources')
P15 = os.path.expanduser('~/pharo-work/harness15/Pharo15.0-64bit-b4d2dfd.sources')
m14, m15 = methods(P14), methods(P15)
print('methods', len(m14), len(m15), file=sys.stderr)
cls14 = {c for c, s in m14}
cls15 = {c for c, s in m15}
by14, by15 = collections.defaultdict(set), collections.defaultdict(set)
for c, s in m14: by14[s].add(c)
for c, s in m15: by15[s].add(c)

def look(m, by, cls, sel):
    if (cls, sel) in m:
        return 'own', m[(cls, sel)]
    return 'inherited', None

def state(cls, sel):
    a, b = m14.get((cls, sel)), m15.get((cls, sel))
    if a is None and b is None:
        # inherited: compare all definers of the selector that exist in both
        d14, d15 = by14.get(sel, set()), by15.get(sel, set())
        if not d14 and not d15:
            return 'not in either'
        if cls not in cls15 and cls not in {c for c, s in m15}:
            pass
        common = d14 & d15
        if common and all(m14[(c, sel)] == m15[(c, sel)] for c in common) and d14 == d15:
            return 'same (inherited)'
        return 'inherited, differs or other definers'
    if b is None:
        return 'gone in 15' if cls not in cls15 else 'method gone in 15'
    if a is None:
        return 'new in 15'
    return 'same' if a == b else 'differs'

rows = [l.rstrip('\n').split('\t') for l in open('/Users/wohl/src/iospharo/docs/results/nonpassing-tests-2026-11-18.tsv')][1:]
out = ['part\tclass\tselector\tgroup\tstate14to15']
c = collections.Counter()
for r in rows:
    if not (r[0].startswith('base') or r[0] == 'found'):
        continue
    st = state(r[1], r[2])
    c[(r[0][:7], st)] += 1
    out.append('\t'.join([r[0], r[1], r[2], r[3], st]))
open(HERE + '/p14p15-tests.tsv', 'w').write('\n'.join(out) + '\n')
for k, v in sorted(c.items()):
    print('tests', k, v)

out = ['entry\tkind\tclass\tselector\tstate14to15']
c2 = collections.Counter()
for f in sorted(glob.glob('/Users/wohl/src/iospharo/scripts/image-fixes/[123]*.st')):
    if '36-' in f:
        continue
    t = open(f).read()
    for m in re.finditer(r"\b(fix|add) value: #(?:'([^']+)'|([A-Za-z0-9_]+)) value: #(?:'([^']+)'|([A-Za-z0-9_:]+))", t):
        kind = m.group(1); cls = m.group(2) or m.group(3); sel = m.group(4) or m.group(5)
        if kind == 'add':
            st = 'add: ' + ('already in 15' if (cls, sel) in m15 else ('class in 15' if any(k[0] == cls for k in m15) else 'class absent in 15'))
        else:
            st = state(cls, sel)
        c2[st] += 1
        out.append('\t'.join([os.path.basename(f), kind, cls, sel, st]))
open(HERE + '/p14p15-fixes.tsv', 'w').write('\n'.join(out) + '\n')
for k, v in sorted(c2.items()):
    print('fix-methods', k, v)
