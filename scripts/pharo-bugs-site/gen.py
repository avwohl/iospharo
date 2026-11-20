#!/usr/bin/env python3
"""Build the static site pharo_bugs1 (https://www.awohl.com/pharo_bugs1/): an
index of the defects behind the tests that did not pass on our VM and on Cog,
and one page per defect.  The site is for the Pharo group: it leaves out the
defects of our VM (docs/nonpassing-tests.md, part D, keeps them).

Inputs: the records of this repository, the saved results in
~/pharo-work-save/2026-11-17, checks-2026-11-18.json (the checks of
2026-11-18: the state of each test before the repairs and the state of each
package row), and trees/ (the file lists of the external projects of
BaselineOfPharo at their versions).  No VM runs.

    python3 scripts/pharo-bugs-site/gen.py
    rsync -az --delete -e 'ssh -p 24' scripts/pharo-bugs-site/out/ wohl@awohl.com:www/pharo_bugs1/
"""
import collections, glob, html, json, os, re, shutil, subprocess, sys
from markdown_it import MarkdownIt

SCR = os.path.dirname(os.path.abspath(__file__))
REPO = '/Users/wohl/src/iospharo'
OUT = SCR + '/out'
SAVE = os.path.expanduser('~/pharo-work-save/2026-11-17')
P14SRC = SAVE + '/images/Pharo14.0-64bit-42a90c7.sources'
DATE = '2026-11-20'

md = MarkdownIt('commonmark', {'html': False}).enable('table')

# ---------------------------------------------------------------- sanitizing
SCRATCH_RE = re.compile(r'/private/tmp/claude-501/[^\s`\'"),]*')
def clean(text):
    text = SCRATCH_RE.sub('<local work directory>', text)
    text = text.replace('/Users/wohl/', '~/').replace('/home/wohl/', '~/')
    text = re.sub(r' ?<(?:xcl2|wohl)@awohl\.com>', '', text)
    text = re.sub(r'xcl2@awohl\.com|wohl@awohl\.com', '', text)
    text = re.sub(r'~/src/iospharo(?:-jit)?/?', 'the iospharo repository ', text)
    text = re.sub(r'~/src/pharo-vm/', 'pharo-vm/', text).replace('~/src/pharo-vm', 'the Cog sources (pharo-project/pharo-vm)')
    text = re.sub(r'~/src/pharo/', 'pharo/', text).replace('~/src/pharo', 'the Pharo sources (pharo-project/pharo)')
    text = re.sub(r'(?<![A-Za-z0-9_])(?:scratch/)?ag/[A-Za-z0-9_./-]+', '<work directory>', text)
    text = re.sub(r'/tmp/pharo-work/[A-Za-z0-9_./-]*', '<work directory>', text)
    text = re.sub(r'(?:docs/)?vm-compat-bugs\.md', 'the iospharo defect list', text)
    text = re.sub(r'~/pharo-work-save/[A-Za-z0-9_./-]*', '<work directory>', text)
    text = text.replace("On the\n   test machine a key was set, and the real service answered",
                        "With a key that\n   the service does not accept, the real service answered")
    return text

ROW75_OLD = re.compile(r"`ClaudeSDKExampleSupport class>>resolveClient` reads\n  the key from .*?x-api-key\"\.", re.S)
ROW75_NEW = ("`ClaudeSDKExampleSupport class>>resolveClient` reads\n  a key from the environment or the keychain, and the tests send real\n"
             "  requests to the service.  Without a valid key the service refuses each\n  request.")
ROW75_TOOLS = re.compile(r"- What we changed in our tools because of this row:.*?\n(?=- Repair here)", re.S)

def clean_md(text):
    text = re.sub(r'\[?(?:docs/)?vm-compat-bugs\.md\]?(?:\([^)]*\))?', 'the iospharo defect list', text)
    text = ROW75_OLD.sub(ROW75_NEW, text)
    text = ROW75_TOOLS.sub('', text)
    text = text.replace('\\n', '\n')
    return clean(text)

def h(s):
    return html.escape(s, quote=True)

def render(mdtext):
    out = md.render(clean_md(mdtext))
    return re.sub(r'<a href="(?!https?:)[^"]*\.md(?:#[^"]*)?">(.*?)</a>', r'\1', out)

def pre(text, cls='code'):
    return f'<pre class="{cls}">{h(clean(text))}</pre>'

def slug(s):
    s = re.sub(r'[^A-Za-z0-9]+', '-', s).strip('-').lower()
    return s[:80]

# ---------------------------------------------------------------- sources
def chunks(text):
    out, cur, i, n = [], [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '!':
            if i + 1 < n and text[i + 1] == '!':
                cur.append('!'); i += 2; continue
            out.append(''.join(cur)); cur = []; i += 1; continue
        cur.append(c); i += 1
    if cur:
        out.append(''.join(cur))
    return out

def selector_of(src):
    s = src.lstrip()
    first = s.split('\n')[0]
    kws = re.findall(r'([A-Za-z_][A-Za-z0-9_]*:)\s*[A-Za-z_][A-Za-z0-9_]*', first)
    if kws and re.match(r'[A-Za-z_][A-Za-z0-9_]*:', s):
        return ''.join(kws)
    m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)', s)
    if m:
        return m.group(1)
    m = re.match(r'([-+*/\\<>=~@%|&?,]+)', s)
    return m.group(1) if m else None

def methods(path):
    text = open(path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n').replace('\r', '\n')
    res, cls = {}, None
    for ch in chunks(text):
        st = ch.strip()
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*(?: class)?) methodsFor: ', st)
        if m:
            cls = m.group(1); continue
        if cls is None:
            continue
        if st == '':
            cls = None; continue
        sel = selector_of(ch)
        if sel:
            res[(cls, sel)] = ch.strip()
    return res

print('reading Pharo 14 sources', file=sys.stderr)
M14 = methods(P14SRC)
BYSEL = collections.defaultdict(list)
for (c, s) in M14:
    BYSEL[s].append(c)

def test_source(cls, sel, context=''):
    if (cls, sel) in M14:
        return cls, M14[(cls, sel)]
    defs = BYSEL.get(sel, [])
    if len(defs) == 1:
        return defs[0], M14[(defs[0], sel)]
    named = [d for d in defs if f'{d}>>{sel}' in context]
    if len(named) == 1:
        return named[0], M14[(named[0], sel)]
    return None, None

IN_IMAGE = {c for (c, s) in M14}

# ---------------------------------------------------------------- results
NONPASS = [l.rstrip('\n').split('\t') for l in open(REPO + '/docs/results/nonpassing-tests-2026-11-18.tsv')]
NP_HDR, NONPASS = NONPASS[0], NONPASS[1:]
WF = json.load(open(SCR + '/checks-2026-11-18.json'))

# ---------------------------------------------------------------- upstream repository of a class
SRC = os.path.expanduser('~/src/pharo/src')
HOME = set(os.path.basename(p).split('.')[0] for p in glob.glob(SRC + '/*/*.class.st') + glob.glob(SRC + '/*/*.trait.st'))
TREES = {}
for f in glob.glob(SCR + '/trees/*.txt'):
    n = os.path.basename(f)[:-4].replace('_', '/', 1)
    for l in open(f):
        m = re.search(r'/([A-Za-z0-9_]+)\.(class|trait)\.st$', l.strip())
        if m:
            TREES.setdefault(m.group(1), set()).add(n)

def upstream_of(c):
    c = c.replace(' class', '')
    if c in HOME or c in ('AtomicSharedQueueTest', 'TKTFutureStateWithRegistrationHook'):
        return 'pharo-project/pharo'
    if c in TREES:
        return ' and '.join(sorted(TREES[c]))
    if c in ('GEllipse', 'GArcTest', 'GEllipseTest'):
        return 'pharo-contributions/Geometry'
    return None

def entry_classes(path):
    t = open(path).read()
    return [m.group(1) or m.group(2) for m in re.finditer(r"\b(?:fix|add) value: #(?:'([^']+)'|([A-Za-z0-9_]+)) value:", t)]

# ---------------------------------------------------------------- page frame
CSS = """
:root{--bg:#fbfbf8;--fg:#1c1c1a;--mut:#5d5d58;--line:#dddcd4;--acc:#1f5f8b;--code:#f1f0ea;
--ok:#2e7d32;--na:#6d6d6d;--open:#b23b1e;--part:#8a6100}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#e6e5df;--mut:#a3a29a;--line:#34342f;--acc:#7fb6dd;--code:#21211f;
--ok:#7cc47f;--na:#a0a09a;--open:#ef8a6e;--part:#e0b350}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
.wrap{max-width:1100px;margin:0 auto;padding:24px 16px 64px}
a{color:var(--acc)} h1{font-size:1.7rem;line-height:1.25;margin:.2em 0 .4em} h2{font-size:1.25rem;margin:1.8em 0 .5em;border-bottom:1px solid var(--line);padding-bottom:.2em}
h3{font-size:1.05rem;margin:1.4em 0 .4em}
.crumb{font-size:.9rem;color:var(--mut)} .lede{color:var(--mut)}
code{font:13.5px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;background:var(--code);padding:.05em .3em;border-radius:3px;overflow-wrap:break-word}
td:first-child{min-width:14em} td code{word-break:normal}
pre{font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;background:var(--code);padding:12px;border-radius:6px;overflow:auto;max-height:600px}
pre code{background:none;padding:0}
table{border-collapse:collapse;width:100%;font-size:.92rem;margin:.6em 0;display:block;overflow-x:auto}
th,td{border:1px solid var(--line);padding:5px 8px;vertical-align:top;text-align:left}
th{background:var(--code)}
.st{display:inline-block;font-size:.8rem;font-weight:600;padding:.05em .55em;border-radius:999px;border:1px solid currentColor;white-space:nowrap}
.st-fixed{color:var(--ok)} .st-na{color:var(--na)} .st-open{color:var(--open)} .st-part{color:var(--part)}
.meta{display:grid;grid-template-columns:max-content 1fr;gap:4px 14px;font-size:.95rem;margin:.6em 0 1em}
.meta dt{color:var(--mut)} .meta dd{margin:0}
.note{border-left:3px solid var(--line);padding:.2em .9em;color:var(--mut)}
details{margin:.5em 0} summary{cursor:pointer;font-weight:600}
.counts{display:flex;flex-wrap:wrap;gap:10px;margin:1em 0}
.counts div{border:1px solid var(--line);border-radius:8px;padding:8px 14px;min-width:150px}
.counts b{display:block;font-size:1.4rem}
"""

def status_badge(st):
    st = re.sub(r'\b1 tests\b', '1 test', st)
    cls = {'Fixed': 'st-fixed', 'Not applicable': 'st-na', 'Open': 'st-open'}.get(st.split(':')[0].split(' (')[0], 'st-part')
    if st.startswith('Fixed') or st.startswith('Partly'):
        cls = 'st-fixed'
        if any(w in st for w in ('not accepted', 'needs', 'not run', 'only in', 'Partly', 'stand-in')):
            cls = 'st-part'
    elif st.startswith('Not applicable'):
        cls = 'st-na'
    elif st.startswith('Open'):
        cls = 'st-open'
    return f'<span class="st {cls}">{h(st)}</span>'

def page(title, body, depth=1, desc=''):
    up = '../' * depth
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{h(title) if title == 'Pharo test defects' else h(title) + ' - Pharo test defects'}</title><meta name="description" content="{h(desc or title)}">
<link rel="stylesheet" href="{up}style.css"></head>
<body><div class="wrap">
<p class="crumb"><a href="{up}index.html">Pharo test defects</a></p>
{body}
<p class="crumb" style="margin-top:3em">State of {DATE}.  Made from the test records of the iospharo project (a second Pharo VM).  The fixes are proposals for review.</p>
</div></body></html>
"""

def meta(rows):
    return '<dl class="meta">' + ''.join(f'<dt>{h(k)}</dt><dd>{v}</dd>' for k, v in rows) + '</dl>'

def filelink(rel, label=None):
    return f'<a href="../files/{h(rel)}">{h(label or rel)}</a>'

FILES = {}  # published path -> source path
def publish_file(src, rel):
    FILES[rel] = src
    return rel

def file_block(src, rel, title=None):
    publish_file(src, rel)
    text = open(src, encoding='utf-8', errors='replace').read()
    t = title or os.path.basename(src)
    return f'<details open><summary>{h(t)} ({filelink(rel, "download")})</summary>{pre(text)}</details>'

PAGES = []  # dict(kind, slug, title, status, tests, fix, where, upstream)

WEB_EXCLUDE_KINDS = {'vm'}
WEB_EXCLUDE_SLUGS = {'open-interpreter-slow-release-tests'}
EXCLUDED = []
def add_page(kind, sl, title, status, ntests, fix, upstream, body, where):
    if kind in WEB_EXCLUDE_KINDS or sl in WEB_EXCLUDE_SLUGS or any(sl.startswith(x) for x in ('pkg-iconography', 'pkg-row-147-')):
        EXCLUDED.append((kind, sl, title))
        return
    status = re.sub(r'\b1 tests\b', '1 test', status)
    PAGES.append(dict(kind=kind, slug=sl, title=title, status=status, tests=ntests, fix=fix, upstream=upstream, where=where))
    full = (f'<h1>{h(title)}</h1>' + meta([('State', status_badge(status)), ('Defect in', h(where)),
            ('Tests', h(str(ntests))), ('Fix', h(fix)), ('Upstream repository', h(upstream or '-'))]) + body)
    open(f'{OUT}/bugs/{sl}.html', 'w').write(page(title, full, 1, f'{title}: {status}'))

def sections(path):
    text = open(path).read()
    parts = re.split(r'^## ', text, flags=re.M)
    return [(p.split('\n', 1)[0].strip(), p.split('\n', 1)[1] if '\n' in p else '') for p in parts[1:]]

def run_snippet(tests):
    lines = []
    for cls, sel in tests:
        lines.append(f"(({cls} selector: #{sel}) run) printString.")
    return pre('"Run in a Playground of a Pharo 14 image (Do it and print each line)."\n' + '\n'.join(lines))

# ================================================================ build
shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT + '/bugs'); os.makedirs(OUT + '/files')
open(OUT + '/style.css', 'w').write(CSS)
publish_file(REPO + '/scripts/image-fixes/00-header.st', 'image-fixes/00-header.st')
publish_file(REPO + '/docs/results/nonpassing-tests-2026-11-18.tsv', 'nonpassing-tests-2026-11-18.tsv')

# ---------------------------------------------------------------- 1. Pharo image defects (repair record)
REC = REPO + '/docs/upstream/pharo-image-repairs-2026-11-17.md'
STOCK_A2 = {('CodeSimulationTest', 'testErrorCodeNotFound'): 'FAIL',
            ('TFUFFICallbackTest', 'testCallbacksInOtherThread'): 'SKIP (the VM stops without the Cog change)',
            ('WriteBarrierTest', 'testMutateByteArrayUsingDoubleAtPut'): 'FAIL',
            ('WriteBarrierTest', 'testMutateByteArrayUsingFloatAtPut'): 'FAIL'}
APPLY = ("<p>How to apply an entries file: file in <code>00-header.st</code> and the entries file as one chunk "
         "(the iospharo script <code>scripts/gen-image-fixes.py</code> makes that chunk).  Each entry checks for the "
         "defect first and prints <code>FIXED</code>, <code>ADDED</code> or <code>SKIP</code>.  "
         + filelink('image-fixes/00-header.st', 'Download 00-header.st') + '.</p>')

for title, body in sections(REC):
    gname = title.split(' (')[0]
    entries = re.findall(r'image-fixes/([^`]+)`', body.split('\n|', 1)[0])
    rows = [r for r in NONPASS if r[3] == title or r[3] == gname]
    states = collections.Counter(r[5] for r in rows)
    review = re.search(r'- Review: ([^\n]*)', body)
    review = review.group(1) if review else ''
    if title.startswith('NA-'):
        status = 'Not applicable'
        if title == 'NA-platform-windows':
            status = 'Partly fixed: 3 fixed, 16 not applicable'
    elif states and set(states) <= {'repaired'}:
        status = 'Fixed (image fix)'
    elif states.get('our VM only') or states.get('needs a VM change'):
        status = 'Fixed in the image; stock Cog needs a Cog change'
    else:
        status = 'Fixed (image fix)'
    if 'NOT accepted' in review:
        status = 'Partly fixed: 3 tests repaired in the image; debugger stepping not repaired; stock Cog needs a Cog change; review not accepted'
    if title == 'NA-fixture-tests':
        status = 'Not applicable (proof incomplete for 10 tests)'
    if title == 'NA-platform-windows':
        status = 'Partly fixed: 3 fixed, 16 not applicable (2 not proven)'
    if title == 'WriteBarrierTest':
        status = 'Fixed in the image; stock Cog needs the Cog change (not sent)'
    if title.startswith('Taskit-andthen') or title.startswith('Znurl') or 'found' in title:
        pass
    ups = set()
    for e in entries:
        p = REPO + '/scripts/image-fixes/' + e
        if os.path.exists(p):
            for c in entry_classes(p):
                u = upstream_of(c)
                if u: ups.add(u)
    for r in rows:
        u = upstream_of(r[1])
        if u: ups.add(u)
    where = 'the test' if all(r[4] == 'test' for r in rows) and rows else 'the Pharo image'
    if title == 'NA-fixture-tests':
        where = 'none: fixture classes that other tests run, skipped by design'
    elif title == 'NA-platform-32bit':
        where = 'none: tests for a 32-bit VM'
    elif title == 'NA-platform-windows':
        where = 'the test (3 tests); the other 16 are for Windows or x86_64 only'
    out = []
    out.append('<h2>Record</h2>' + render(body))
    if rows:
        out.append('<h2>Results</h2><p>Last full sweeps of 2026-11-17: iospharo (<code>sweep14-new14</code>) and stock Cog with the Cog changes '
                   '(<code>sweepcogp14-new10</code>), with all image fixes.  "Before" is the state of the test before the repairs.  '
                   'The Stock Cog column has a value only where the record measured stock Cog: "PASS 3 of 3 (repair run)" is the check of each repair '
                   'on stock Cog v12.0.6 with the fix entry; "-" is not measured separately.</p>')
        trs = ['<tr><th>Test</th><th>Before</th><th>State</th><th>iospharo VM</th><th>Patched Cog</th><th>Stock Cog</th></tr>']
        for r in rows:
            before = r[10] if len(r) > 10 else ''
            stock = STOCK_A2.get((r[1], r[2]), 'PASS 3 of 3 (repair run)' if (r[5] == 'repaired' and r[0] != 'found') else '-')
            trs.append(f'<tr><td><code>{h(r[1])}&gt;&gt;{h(r[2])}</code></td><td>{h(before)}</td><td>{h(r[5])}</td>'
                       f'<td>{h(r[6])}</td><td>{h(r[7])}</td><td>{h(stock)}</td></tr>')
        out.append('<table>' + ''.join(trs) + '</table>')
    tests = sorted({(r[1], r[2]) for r in rows})
    if tests:
        ext_cls = sorted({c for c, s in tests if c not in IN_IMAGE})
        if title.startswith('NA-'):
            msg = 'Run the tests in a Pharo 14 image on macOS arm64: they skip (the fixtures and the platform tests skip by design).'
        elif any(r[0] == 'found' for r in rows):
            msg = ('Run the tests in a Pharo 14 image (build 791).  Some of these tests are new: the entries file adds them, so file in the '
                   'entries file first, then run them.  ')
            if ext_cls:
                msg += 'These classes are not in the Pharo image but in a package (see the record): ' + ', '.join(ext_cls) + '.'
        else:
            msg = 'Run the tests in a Pharo 14 image (build 791).  Before the fix they skip, fail or carry the expected-failure mark.'
        out.append('<h2>Test</h2><p>' + h(msg) + '</p>')
        out.append(run_snippet(tests[:40]))
        shown = set()
        srcs = []
        for cls, sel in tests:
            d, s = test_source(cls, sel, body)
            if s and (d, sel) not in shown:
                shown.add((d, sel))
                srcs.append(f'<details><summary>{h(d)}&gt;&gt;{h(sel)} before the fix (Pharo 14 build 791)</summary>{pre(s)}</details>')
        if srcs:
            out.append('<h3>Source of the tests before the fix</h3>' + ''.join(srcs[:40]))
    EXTRA_ENTRIES = {'WriteBarrierTest': ['21-write-barrier-integer-stores.st'], 'TFFI-disabled-tests': ['30-ffi-callback-from-other-thread.st']}
    entries = entries + [e for e in EXTRA_ENTRIES.get(gname, []) if e not in entries]
    OPEN_NOTES = {
        'NA-fixture-tests': 'Open: <code>AllTestFailTest</code> (4 tests) and <code>DependencyTest</code> (6 tests) have no other test that runs them.  A skip is accepted only for a fixture that another test runs, so these 10 tests are still open.',
        'NA-platform-windows': 'Open: the two tests <code>testCallingFunctionWithW64CallingConvention</code> (<code>TFFunctionCallTest</code> and <code>TFUFFIDifferentCallingConventionFunctionCallTest</code>) run only on an x86_64 VM on macOS.  The proof is only the architecture name on arm64; a run on an x86_64 VM under Rosetta is not done.',
        'TFFI-disabled-tests': 'The record text below was written before entry 30.  Entry 20 fixes 3 of the 4 tests.  <code>TFUFFICallbackTest&gt;&gt;testCallbacksInOtherThread</code> is fixed by entry 30 together with the Cog change for a callback from another thread (see the Cog section); on stock Cog without the change the test still skips for the same-thread runner, because the VM would stop.',
        'WriteBarrierTest': 'The two tests pass on stock Cog only with the Cog change for the typed store primitives (see the Cog section).  Entry 21 adds two tests for the integer stores.',
        'Debugger-kernel-skips': 'Open: the debugger still steps with the old stop points (<code>shouldStepStopAt:</code>), because 11 tests of the external Sindarin package (<code>SindarinDebuggerTest</code>) fail with the new stop points.  A decision is needed: change Sindarin, or keep the two stop sets.  The test <code>CodeSimulationTest&gt;&gt;testErrorCodeNotFound</code> passes on stock Cog only with the Cog change for primitive 218 (see the Cog section).',
    }
    if gname in OPEN_NOTES:
        out.insert(0, '<p class="note">' + OPEN_NOTES[gname] + '</p>')
    if entries:
        out.append('<h2>Fix</h2>' + APPLY)
        for e in entries:
            p = REPO + '/scripts/image-fixes/' + e
            if os.path.exists(p):
                out.append(file_block(p, 'image-fixes/' + e))
    else:
        out.append('<h2>Fix</h2><p>No fix: the tests cannot apply on this machine (proof in the record above).</p>')
    nt = len(tests)
    fix = ('image fix ' + ', '.join(entries)) if entries else 'none'
    add_page('image', 'image-' + slug(gname), gname, status, nt, fix, ' and '.join(sorted(ups)), ''.join(out), where)

# interactive-session group (A4)
rows = [r for r in NONPASS if r[0] == 'base-A4']
e22 = REPO + '/scripts/image-fixes/22-interactive-session-tests.st'
body = ['<h2>Summary</h2><p>28 tests skip when the test run is not an interactive session (no real display).  Six of them asked '
        '<code>Smalltalk isHeadless</code>, which always answers true in Pharo 14, so they never ran on any VM.  '
        'Two tests had defects that showed once they ran: <code>TaskbarMorph&gt;&gt;updateTasks</code> sends '
        '<code>hasProperty:</code> to a <code>MorphicNativeWindow</code> (a DNU on our VM and on stock Cog), and '
        '<code>SimulateMouseTest&gt;&gt;testSimulateRightClick</code> right-clicks a presenter instead of its morph.  '
        'With the fix, 4 tests pass in a normal run, and the other 24 pass in an interactive run of our VM (135 of 135 tests of the six classes).  '
        'No interactive run on Cog is recorded.</p>']
trs = ['<tr><th>Test</th><th>Before</th><th>iospharo, normal run</th><th>Patched Cog, normal run</th><th>iospharo, interactive run</th></tr>']
for r in rows:
    trs.append(f'<tr><td><code>{h(r[1])}&gt;&gt;{h(r[2])}</code></td><td>{h(r[10])}</td><td>{h(r[6])}</td><td>{h(r[7])}</td><td>{h(r[8])}</td></tr>')
body.append('<h2>Results</h2><table>' + ''.join(trs) + '</table>')
body.append('<h2>Test</h2>' + run_snippet([(r[1], r[2]) for r in rows]) + '<p>Start the image with a display (stock Cog: no <code>--headless</code>; iospharo: <code>PHARO_INTERACTIVE=1</code>).</p>')
body.append('<h2>Fix</h2>' + APPLY + file_block(e22, 'image-fixes/22-interactive-session-tests.st'))
body.insert(0, '<p class="note">Stock Cog stopped with a segmentation fault in 1 of 3 interactive runs of <code>MorphicWindowManagerTest</code>: see <a href="cog-morphic-window-manager-segfault.html">that Cog page</a>.</p>')
add_page('image', 'image-interactive-session-tests', 'Tests that need an interactive session', 'Partly fixed: 4 pass in a normal run; 24 pass only in an interactive run of iospharo (not run on Cog)',
         28, 'image fix 22-interactive-session-tests.st', 'pharo-project/pharo and pharo-spec/Spec', ''.join(body), 'the Pharo image and the tests')

# entry 23 (defects the standard runner shows) has no record section
e23 = REPO + '/scripts/image-fixes/23-defects-the-standard-runner-shows.st'
if os.path.exists(e23):
    add_page('image', 'image-standard-runner-defects', 'Defects that the standard SUnit runner shows', 'Fixed (image fix)', 0,
             'image fix 23-defects-the-standard-runner-shows.st', 'pharo-project/pharo',
             '<h2>Summary</h2><p><code>OutOfMemoryTest&gt;&gt;setUp</code> installs the Low Space Watcher, and the manifest of '
             '<code>Collections-Unordered</code> names a dependency that is out of date.  The two defects show with the standard '
             'SUnit runner (<code>runCaseManaged</code>).  The comment of the entries file has the detail.</p>'
             '<h2>Fix</h2>' + APPLY + file_block(e23, 'image-fixes/23-defects-the-standard-runner-shows.st'), 'the Pharo image')

e24 = REPO + '/scripts/image-fixes/24-stale-executable-comment.st'
if os.path.exists(e24):
    hdr = open(e24).read().split('"', 2)[1] if open(e24).read().startswith('"') else ''
    add_page('image', 'image-stale-executable-comment', 'A stale comment of Context class>>tallyInstructions:', 'Fixed (image fix)', 1,
             'image fix 24-stale-executable-comment.st', 'pharo-project/pharo',
             '<h2>Summary</h2>' + (pre(hdr) if hdr else '<p>See the comment of the entries file.</p>')
             + '<h2>Test</h2>' + run_snippet([('DocCommentsReleaseTest', 'testExecutableCommentsAreValid')])
             + '<h2>Fix</h2>' + APPLY + file_block(e24, 'image-fixes/24-stale-executable-comment.st'), 'the Pharo image')

# ---------------------------------------------------------------- 2. open items of the base suite
d36 = REPO + '/scripts/image-fixes/36-taskit-newprocess-runner-tests.st'
b = ['<h2>Summary</h2><p><code>TKTNewProcessTaskRunnerTest&gt;&gt;testNewProcessTaskRunnerRunsInNewProcessEveryTime</code> failed 1 time in the '
     'sweep <code>sweep14-new14</code> of our VM ("Got 9 instead of 10").  It passed in the 5 other saved sweeps of 2026-11-17 '
     '(3 of our VM, 2 of the patched Cog).  The source of the test says that it fails at random on the CI of Pharo.  The cause is not proven.</p>',
     '<h2>Test</h2>' + run_snippet([('TKTNewProcessTaskRunnerTest', 'testNewProcessTaskRunnerRunsInNewProcessEveryTime')])]
s = test_source('TKTNewProcessTaskRunnerTest', 'testNewProcessTaskRunnerRunsInNewProcessEveryTime')[1]
if s:
    b.append(pre(s))
b.append('<h2>Fix</h2><p>None yet.  A draft of a test change exists in the iospharo working tree.  The draft is not validated, and its cause is a hypothesis, so it is not published here.</p>')
add_page('open', 'open-taskit-newprocess-runner', 'TaskIt: a new-process task runner test fails at random', 'Open: draft fix, not validated',
         1, 'draft only', 'pharo-project/pharo', ''.join(b), 'the Pharo image or the test (not proven)')

PARTC = [
    ('open-stspotter-preview-once', 'Spotter: testOpenSpotterRefreshesPreviewOnce fails at times',
     [('StSpotterTest', 'testOpenSpotterRefreshesPreviewOnce')],
     'Failed in 1 of 4 sweeps of <code>build-nojit</code> (Pharo 14) and in 1 of 4 sweeps before that; stock Cog fails it too.  It passes in the 6 saved sweeps of 2026-11-17.',
     'pharo-spec/NewTools'),
    ('open-stplaygroundpage-contents', 'Playground: StPlaygroundPageTest>>testContents fails at times',
     [('StPlaygroundPageTest', 'testContents')],
     'Failed in earlier sweeps of Pharo 14; the records call it timing-dependent.  It passes in the 6 saved sweeps of 2026-11-17.',
     'pharo-spec/NewTools'),
    ('open-freetype-cache-race', 'FreeType cache: a race on the lazy FreeType face',
     [('FreeTypeCacheTest', 'testGlyphAccessIsThreadSafe'), ('FreeTypeCacheTest', 'testGlyphAccessIsThreadSafeWhenAskingWidth')],
     'Pharo 14 arm64: our VM failed 11 of 50 runs, stock Cog 9 of 50 runs.  Pharo 13 shows it too.  The records name a race in the image on the lazy FreeType face.  Both tests pass in the 6 saved sweeps of 2026-11-17.',
     'pharo-project/pharo'),
    ('open-transcript-presenter-updatetabs', 'Transcript presenter: two processes run TabSelectorMorph>>updateTabs on one morph',
     [('StObjectTranscriptPresenterTest', 'testOpenedLogIsSelectedLog')],
     'Pharo 14 x86_64: about 3 failures in 1000 runs on our VM and on Cog.  The test process and the Morphic UI process, both at priority 40, run <code>TabSelectorMorph&gt;&gt;updateTabs</code> on one morph.  A gate in the fake GUI of the test harness (no World cycle of another process while the test process is ready) is in test; it is a change of the harness, not of the image.',
     'pharo-spec/NewTools'),
    ('open-interpreter-slow-release-tests', 'JIT off: two code-scan tests run past the time limit',
     [('NoUnusedVariablesLeftTest', 'testNoUnusedTemporaryVariablesLeft'), ('ReleaseTest', 'testNoShadowedVariablesInMethods')],
     'On <code>build-nojit</code> (our VM with the JIT off, the iOS configuration) the tests took 73 s and 270 s and hit the 80 s limit of the old runner.  They pass with the JIT on.  No sweep of <code>build-nojit</code> ran after 2026-11-15.',
     'iospharo (VM speed with the JIT off)'),
]
for sl, title, tests, text, up in PARTC:
    b = ['<h2>Summary</h2><p>' + text + '  No fix exists.  The rule of the project counts a test that fails at times as a test that fails.</p>',
         '<h2>Test</h2>' + run_snippet(tests)]
    for c, s in tests:
        src = test_source(c, s)[1]
        if src:
            b.append(f'<details><summary>{h(c)}&gt;&gt;{h(s)}</summary>{pre(src)}</details>')
    b.append('<h2>Fix</h2><p>None yet.</p>')
    add_page('open', sl, title, 'Open: no fix', len(tests), 'none', up, ''.join(b), 'the Pharo image or the test (not proven)')

b = ('<h2>Summary</h2><p>The first sweep of Pharo 13 with the old runner gave 27894 passes of 28077 tests: 2 failures, 131 skips and 50 '
     'expected failures (183 tests that do not pass).  No per-test list of that sweep was saved, and the repair work for Pharo 13 did not start.  '
     'A comparison of the sources shows that 123 of the 132 Pharo 14 repairs have the same defect in Pharo 13; 8 of the tests are not in '
     'Pharo 13, and 1 skip exists only in Pharo 14.  Seven Pharo 13 test defects have their own fix (below).</p>'
     '<p>Of the methods that the fix entries 20 to 35 replace, 126 have the same source in Pharo 13, 10 differ and 16 are not in Pharo 13.  '
     'An entry checks the old source text first, so an entry for a method that differs makes no change on Pharo 13.  The 10 methods that differ: '
     '<code>BlockClosureTest&gt;&gt;testOnForkErrorTakesLessThanOneSecond</code>, <code>ClapHelloTest&gt;&gt;testHelloBothShoutingAndWhispering</code>, '
     '<code>ClyBrowserMorph&gt;&gt;spawnQueryBrowserOn:withState:</code>, <code>OCASTTranslator&gt;&gt;visitArrayNode:</code>, '
     '<code>ReflectivityOnStackTest&gt;&gt;testExecuteWithWaiting...ChangeMethodManually</code>, <code>OCCodeReparator&gt;&gt;openMenu</code>, '
     '<code>NetNameResolver class&gt;&gt;localHostAddress</code>, <code>OCCodeSnippet class&gt;&gt;badExpressions</code>, '
     '<code>RBNamespace&gt;&gt;renameClass:to:around:</code>, <code>RBTransformationsTest&gt;&gt;testRenameClassTransform</code>.  '
     'No Pharo 13 run with the entries is recorded.</p>'
     '<h2>Fix</h2>' + APPLY + file_block(REPO + '/scripts/image-fixes/10-pharo13-test-defects.st', 'image-fixes/10-pharo13-test-defects.st'))
add_page('open', 'open-pharo13-suite', 'Pharo 13: 183 tests do not pass, no per-test list', 'Open: not measured again', 183,
         'partial: 10-pharo13-test-defects.st', 'pharo-project/pharo', b, 'not known per test')

# ---------------------------------------------------------------- 3. Cog
COG = REPO + '/docs/upstream/cog-changes.md'
readme = open(REPO + '/docs/upstream/README.md').read()
patch_of = {}
for m in re.finditer(r'^\| (.+?) \| `([^`]+)` \| `([^`]+\.patch)` \| ([^|]+) \|', readme, re.M):
    patch_of[re.sub(r'[`*]', '', m.group(1)).strip().lower()[:30]] = (m.group(2), m.group(3), m.group(4).strip())
for title, body in sections(COG):
    key = re.sub(r'[`*]', '', title).strip().lower()[:30]
    branch, patch, state = patch_of.get(key, (None, None, None))
    if patch is None:
        for k, v in patch_of.items():
            if k[:18] == key[:18]:
                branch, patch, state = v
    probes = sorted(set(re.findall(r'cogdiff/([A-Za-z0-9_]+\.st)', body)))
    out = ['<h2>Record</h2>' + render(body)]
    tests = re.findall(r'`([A-Z][A-Za-z0-9]*Test)>>(test[A-Za-z0-9_]*)`', body)
    COG_TEST = {
        'socketplugin: local address fr': 'NetNameResolver primLocalAddress printString.\n"Stock Cog on macOS answers #[0 0 0 0].  With the change: the address of an interface that is up."',
        'typed store primitives respect': 'g := ByteArray new: 8.  g beReadOnlyObject.\n[ g float64AtOffset: 0 put: 2.5 ] on: ModificationForbidden do: [ :e | e return: #forbidden ].\ng printString.\n"Stock Cog: the store changes g to #[0 0 0 0 0 0 4 64].  With the change: ModificationForbidden, and g stays all zero."',
        'primitivedonamedprimitivewitha': '| r |\nr := CodeSimulationTest new veryBasicAt: 1.\n{ r. (Context sender: nil receiver: nil method: Object >> #at: arguments: #(10)) push: nil; push: 500;\n\tdoPrimitive: 117 method: CodeSimulationTest >> #veryBasicAt: receiver: CodeSimulationTest new args: #(999) }\n"Stock Cog: {#\'not found\'. {token. nil}}: the simulation loses the error code.  With the change the two agree."',
    }
    snip = next((v for k, v in COG_TEST.items() if key.startswith(k[:25])), None)
    tests = [x for x in tests if x[0] in IN_IMAGE]
    if probes or tests or snip:
        out.append('<h2>Test</h2>')
        if snip:
            out.append(pre('"Run in a Playground of a Pharo 14 image."\n' + snip))
        elif tests:
            out.append(run_snippet(sorted(set(tests))))
        if key.startswith('cogit: a followed literal goes'[:25]):
            out.append('<p>The crash needs a sequence (a become, a compaction of the code zone, then a full GC).  It showed in 5 of 9 runs of the '
                       'tests of the PharoMisc package (package row 6); with the change, 10 of 10 runs were clean.  The record has the steps.</p>')
        for p in probes:
            pp = REPO + '/scripts/cogdiff/' + p
            if os.path.exists(pp):
                out.append('<p>A probe that prints the same lines on a correct VM.  Run it with <code>Pharo.image eval</code> or file it in:</p>'
                           + file_block(pp, 'cogdiff/' + p))
    COG_EXTRA = {'typed store primitives respect': ['21-write-barrier-integer-stores.st', '20-WriteBarrierTest.st'],
                 'ffi: a callback from another th': ['30-ffi-callback-from-other-thread.st']}
    for k, es in COG_EXTRA.items():
        if key.startswith(k[:25]):
            out.append('<h2>Image side</h2><p>The tests that show the defect, with the image fix entries:</p>' + APPLY
                       + ''.join(file_block(REPO + '/scripts/image-fixes/' + e, 'image-fixes/' + e) for e in es))
    out.append('<p class="note">Names such as <code>scripts/vmrun.sh</code> and <code>scripts/cogdiff/</code> are scripts of the '
               '<a href="https://github.com/avwohl/iospharo">iospharo repository</a>, which runs one VM at a time.  '
               '<i>cog</i> is stock Cog, <i>cogp</i> is Cog with the change, <i>ours</i> is iospharo.</p>')
    if patch and os.path.exists(REPO + '/docs/upstream/' + patch):
        out.append(f'<h2>Fix</h2><p>A change of the Cog sources (<code>pharo-project/pharo-vm</code>, branch <code>pharo-12</code>), '
                   f'on the local branch <code>{h(branch)}</code>.  State: {h(state)}.  Not sent upstream.</p>'
                   + file_block(REPO + '/docs/upstream/' + patch, 'cog/' + patch))
        status = 'Fixed in a Cog patch (not sent)'
        if 'not committed' in (state or ''):
            status = 'Patch ready, needs the owner (version pin)'
    else:
        status = 'Open'
    add_page('cog', 'cog-' + slug(title), 'Cog: ' + re.sub(r'`', '', title), status, len(set(tests)), patch or 'none',
             'pharo-project/pharo-vm', ''.join(out), 'stock Cog VM v12.0.6')

COG_OPEN = [
    ('cog-gc-segfault-tkthreadtest', 'Cog: segmentation fault in the garbage collector after the PharoMisc socket tests',
     '<p>Stock Cog stops in 2 of 6 runs of the PharoMisc package row (<code>TkThreadTest&gt;&gt;testFinalization</code>, after the multicast '
     'tests bind three sockets to one port).  The mark phase of a full GC follows a field of an old object that names an address in new '
     'space.  The cause is not known.  (A different GC crash in the same row, in <code>Cogit&gt;&gt;indexForSelector:in:at:</code>, has a patch: see the store-check page.)</p>',
     [('TkThreadTest', 'testFinalization')]),
    ('cog-morphic-window-manager-segfault', 'Cog: segmentation fault in MorphicWindowManagerTest in an interactive session',
     '<p>Stock Cog stopped with a segmentation fault in 1 of 3 interactive runs of <code>MorphicWindowManagerTest</code>.  Not investigated yet.</p>',
     [('MorphicWindowManagerTest', 'testDeleteAllWindowToLeftOfAWindow')]),
]
for sl, title, text, tests in COG_OPEN:
    how = {'cog-gc-segfault-tkthreadtest': 'Load the PharoMisc package (package row 6, <code>bouraqadi/PharoMisc</code>) and run its tests; stock Cog stopped in 2 of 6 runs.',
           'cog-morphic-window-manager-segfault': 'Run the class <code>MorphicWindowManagerTest</code> in an interactive session (stock Cog without <code>--headless</code>); stock Cog stopped in 1 of 3 runs.'}[sl]
    add_page('cog', sl, title, 'Open: cause not known', len(tests), 'none', 'pharo-project/pharo-vm',
             '<h2>Summary</h2>' + text + '<h2>Test</h2><p>' + how + '</p><h2>Fix</h2><p>None yet.</p>', 'stock Cog VM v12.0.6')

# ---------------------------------------------------------------- 4. our VM
VMB = REPO + '/docs/vm-compat-bugs.md'
for title, body in sections(VMB):
    if '2026-11-17' not in title:
        continue
    st = 'Open' if title.startswith('OPEN') else 'Fixed in the iospharo VM'
    t = re.sub(r'^(OPEN|FIXED) 2026-11-17 [—-]+ ', '', title)
    t = t[0].upper() + t[1:]
    out = ['<h2>Record</h2>' + render(body)]
    probes = sorted(set(re.findall(r'cogdiff/([A-Za-z0-9_]+\.st)', body)))
    for p in probes:
        pp = REPO + '/scripts/cogdiff/' + p
        if os.path.exists(pp):
            out.append('<h2>Test</h2>' + file_block(pp, 'cogdiff/' + p))
    out.append('<h2>Fix</h2><p>' + ('The change is in the iospharo VM (<code>main</code>); the record names the files.' if st.startswith('Fixed')
                                    else 'Not fixed yet; the record has the leads.') + '</p>')
    add_page('vm', 'vm-' + slug(t), 'iospharo VM: ' + t, st, 0, 'iospharo commit' if st.startswith('Fixed') else 'none',
             'avwohl/iospharo', ''.join(out), 'the iospharo VM')

VM_EXTRA = [
    ('vm-eval-mode-deferred-start-actions', 'iospharo VM: the eval mode ran its text before the deferred start actions of the image',
     'The image loads the start script of the eval mode inside the first deferred start action; stock Cog forks a process for the eval text, '
     'which runs after each action.  Our VM ran the eval text first, so the servers of Zinc and of packages had not started.  Found by '
     'PharoSmalltalkInteropServer (44 of 44 on Cog, 9 on our VM).  Fixed in <code>src/vm/test_load_image.cpp</code> (commit <code>135e59e6</code>): '
     'the start script runs the deferred actions before the eval text.  Row 137 then passed 44 of 44 on the two VMs.'),
    ('vm-newfileplugin-missing', 'iospharo VM: no NewFilePlugin',
     'Our VM had no <code>NewFilePlugin</code>, so 8 tests of the pharo-newfile package skipped on our VM only.  The POSIX side of the plugin is ported '
     'and reviewed two times; the row passes 42 of 42 on the two VMs.  The same work found defects in the stock plugin (see the Cog page about the NewFilePlugin).'),
    ('vm-parameter-11-tenure-count', 'iospharo VM: VM parameter 11 (objects tenured by scavenges) was always 0',
     'A test of the Stargate package asserts a positive value.  Fixed in <code>ObjectMemory</code> and <code>Primitives</code>; the sweep <code>sweep14-new12</code> is clean.'),
]
for sl, title, text in VM_EXTRA:
    add_page('vm', sl, title, 'Fixed in the iospharo VM', 0, 'iospharo commit', 'avwohl/iospharo',
             '<h2>Summary</h2><p>' + text + '</p>', 'the iospharo VM')

# ---------------------------------------------------------------- 5. packages
PKG = REPO + '/docs/upstream/package-repairs.md'
manifest = [l.rstrip('\n').split('\t') for l in open(REPO + '/scripts/pkg-jit-test/packages-200.tsv')][1:]
label_of = {i + 1: r[0] for i, r in enumerate(manifest)}
row_of = {v: k for k, v in label_of.items()}
summary = {}
for l in open(SAVE + '/pkgs1/summary.tsv'):
    f = l.rstrip('\n').split('\t')
    summary[f[0]] = f
wfrows = {r['row']: r for r in WF['pkg']['rows']}
open_tests = collections.defaultdict(list)
for r in NONPASS:
    if r[0].startswith('pkg-row-'):
        open_tests[int(r[0][8:])].append(r)
PK = REPO + '/scripts/pkg-jit-test/'

AOC_LABELS = {'terryc321-pharo-advent-of-code', 'shnarazk-aoc-in-pharo'}
def pkg_named_files(text):
    out = []
    for sub, what in [('fixes', 'Fixes file'), ('patches', 'Patch of the package source'), ('not-applicable', 'Tests that cannot apply'),
                      ('fixtures', 'Fixtures'), ('locks', 'Version locks')]:
        for p in sorted(glob.glob(f'{PK}{sub}/*')):
            b = os.path.basename(p)
            if b in text and not any(b.startswith(a) for a in AOC_LABELS if sub == 'fixes'):
                out.append((what, p, f'packages/{sub}/{b}'))
    return out

def pkg_files(label):
    out = []
    for sub, ext, what in [('fixes', '.st', 'Fixes file (filed in after the load)'), ('patches', '.patch', 'Patch of the package source'),
                           ('not-applicable', '.txt', 'Tests that cannot apply, with the proof'), ('fixtures', '.txt', 'Fixtures'),
                           ('locks', '.txt', 'Version locks')]:
        for p in sorted(glob.glob(f'{PK}{sub}/{label}{ext}')):
            if label in AOC_LABELS and sub == 'fixes':
                continue
            out.append((what, p, f'packages/{sub}/{os.path.basename(p)}'))
    return out

def pkg_state(row):
    f = summary.get(label_of.get(row, ''))
    if not f:
        return None
    return f

done_rows = set()
for title, body in sections(PKG):
    rows = [int(x) for x in re.findall(r'\d+', re.search(r'rows? ([\d, and]+)\)', title).group(1))] if re.search(r'rows? ([\d, and]+)\)', title) else []
    done_rows.update(rows)
    labels = [label_of[r] for r in rows if r in label_of]
    out = ['<h2>Record</h2>' + render(body)]
    res = []
    for r in rows:
        f = pkg_state(r)
        if f:
            res.append(f'<tr><td>{r}</td><td><code>{h(f[0])}</code></td><td>{h(f[1])}</td><td>{h(f[3])}</td><td>{h(f[4])}</td></tr>')
    if res:
        out.append('<h2>Results</h2><p>Last result of the package run (stock Cog and iospharo on the same image).</p><table><tr><th>Row</th><th>Label</th><th>Load</th><th>Stock Cog</th><th>iospharo VM</th></tr>'
                   + ''.join(res) + '</table>')
    ot = [t for r in rows for t in open_tests.get(r, [])]
    if ot:
        trs = ''.join(f'<tr><td><code>{h(t[1])}&gt;&gt;{h(t[2])}</code></td><td>{h(t[7])}</td><td>{h(t[6])}</td></tr>' for t in ot)
        out.append('<h3>Tests that do not pass</h3><table><tr><th>Test</th><th>Stock Cog</th><th>iospharo VM</th></tr>' + trs + '</table>')
    fl = [x for lab in labels for x in pkg_files(lab)]
    seen = {x[1] for x in fl}
    fl += [x for x in pkg_named_files(body) if x[1] not in seen]
    if any(l in AOC_LABELS for l in labels):
        out.append('<p class="note">The fixes file of this row is not shown here: it has values of the puzzle input and answers of '
                   'the package author.  It is in the iospharo repository, <code>scripts/pkg-jit-test/fixes/</code>.</p>')
    if fl:
        out.append('<h2>Fix</h2>' + ''.join(file_block(p, rel, what + ': ' + os.path.basename(p)) for what, p, rel in fl))
    else:
        out.append('<h2>Fix</h2><p>No fixes file; the record above says what changed.</p>')
    # status
    wst = [wfrows[r] for r in rows if r in wfrows]
    if any(f is not None and f[1].startswith('LOAD-FAIL') for f in (pkg_state(r) for r in rows)) and not ot:
        status = 'Open: the package does not load'
    elif any(w['open_tests'] > 0 for w in wst):
        status = 'Open: %d tests' % sum(w['open_tests'] for w in wst)
        if any(w['blocked_on_owner'] for w in wst):
            status += ', a decision of the owner'
    elif ot:
        status = 'Fixed; %d tests cannot apply' % len(ot)
    else:
        status = 'Fixed (package repair)'
    name = title.split(' (')[0]
    ntest = len(ot)
    loads = [(r, manifest[r - 1][-1]) for r in rows if r in label_of]
    if loads:
        out.insert(1, '<h2>Test</h2><p>Load the package into a Pharo 14 image (build 791) with the load expression of the test run, file in the fixes file '
                   'of the row if there is one, and run the test classes of the package.</p>'
                   + ''.join(pre(f'"row {r}"\n' + le) for r, le in loads))
    out.insert(0, '<p class="note">The record is the text of the repair.  The Results table is the last run of the row.</p>')
    if any(l in ('gabriel-darbord-opentelemetry-pharo', 'punt-labs-anthropic-sdk-pharo') for l in labels):
        status += ' (the tests use a stand-in server in the fixes file)'
    up = ', '.join(sorted(set(re.findall(r'`([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)`', title)))) or '-'
    add_page('pkg', 'pkg-' + slug(name), ('Packages: ' if len(rows) > 1 else 'Package: ') + title.replace('`', ''), status, ntest,
             ', '.join(os.path.basename(p) for _, p, _ in fl if p.endswith(('.st', '.patch'))) or 'see record', up, ''.join(out), 'see the record')

# rows with open tests and no section in the record
for row, w in sorted(wfrows.items()):
    if row in done_rows:
        continue
    lab = label_of[row]
    ot = open_tests.get(row, [])
    trs = ''.join(f'<tr><td><code>{h(t[1])}&gt;&gt;{h(t[2])}</code></td><td>{h(t[7])}</td><td>{h(t[6])}</td></tr>' for t in ot)
    f = pkg_state(row)
    out = [f'<h2>Summary</h2><p>{h(clean(w["state"]))}</p><p><b>Next step:</b> {h(clean(w["next_step"]))}</p>']
    if f:
        out.append(f'<h2>Results</h2><table><tr><th>Load</th><th>Stock Cog</th><th>iospharo VM</th></tr><tr><td>{h(f[1])}</td><td>{h(f[3])}</td><td>{h(f[4])}</td></tr></table>')
    out.append('<h2>Test</h2><table><tr><th>Test</th><th>Stock Cog</th><th>iospharo VM</th></tr>' + trs + '</table>')
    fl = pkg_files(lab)
    out.append('<h2>Fix</h2>' + (''.join(file_block(p, rel, what + ': ' + os.path.basename(p)) for what, p, rel in fl) if fl else '<p>None yet.</p>'))
    st = 'Open: %d tests' % w['open_tests'] if w['open_tests'] else 'Fixed; %d tests cannot apply' % w['not_applicable_tests']
    if w['blocked_on_owner']:
        st += ', a decision of the owner'
    if row in (151, 153, 154):
        st += ' (a repair is written, not run)'
    elif row in (138, 146):
        st += ' (the first repair ran; a second draft is not in the repository)'
    add_page('pkg', 'pkg-row-%d-%s' % (row, slug(lab)), 'Package row %d: %s' % (row, lab), st, len(ot),
             ', '.join(os.path.basename(p) for _, p, _ in fl) or 'none', manifest[row - 1][1] + '/' + manifest[row - 1][2], ''.join(out),
             'the package (cause not proven for each test)')

# load failures without a section (rows 155 to 157)
for row in (155, 156, 157):
    if row in done_rows or row in wfrows:
        continue
    lab = label_of[row]
    txt = {155: 'The manifest loaded the branch Pharo13 into a Pharo 14 image (SpContainerStyle did not understand #intercellSpacing:).  The repair (branch Pharo14) is committed and did not run.',
           156: 'The branch Pharo13 has no package NewTools-Refactoring-Commands.  The repair (tag v0.11.0, the tag of the image) is committed and did not run.',
           157: "The baseline has no group 'tests'.  The repair (load: #('default')) is committed and did not run."}[row]
    add_page('pkg', 'pkg-row-%d-%s' % (row, slug(lab)), 'Package row %d: %s' % (row, lab), 'Open: the package does not load (repair not run)', 0,
             'manifest change (commit f433b127)', manifest[row - 1][1] + '/' + manifest[row - 1][2],
             '<h2>Summary</h2><p>' + h(txt) + '  The run needs <code>LOAD_TIMEOUT=2400 TEST_TIMEOUT=3600</code>.</p>', 'the package manifest of the test run')

# ---------------------------------------------------------------- files
written = ''.join(open(f).read() for f in glob.glob(OUT + '/bugs/*.html'))
for rel, src in list(FILES.items()):
    if rel != 'nonpassing-tests-2026-11-18.tsv' and ('files/' + rel) not in written:
        del FILES[rel]
        continue
    dst = OUT + '/files/' + rel
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    data = open(src, encoding='utf-8', errors='replace').read()
    open(dst, 'w').write(clean(data))

# ---------------------------------------------------------------- index
GH = 'https://github.com/pharo-project'
CI_SKIP = [('RandomTest', 'testUnixRandomGeneratorSeed', ''), ('AllocationTest', 'testOneGBAllocation', ' (on Windows only)'),
           ('AllocationTest', 'testOneGWordAllocation', ''), ('OmDeferrerTest', 'testExecuteOnceAfterScheduling', ''),
           ('OmDeferrerTest', 'testExecuteWithoutDelay', ''), ('WeakAnnouncerTest', 'testNoDeadWeakSubscriptions', ''),
           ('DebugPointTest', 'testTranscriptDebugPoint', ''),
           ('TKTNewProcessTaskRunnerTest', 'testNewProcessTaskRunnerRunsInNewProcessEveryTime', ' (fails at random: <a href="bugs/open-taskit-newprocess-runner.html">page</a>)')]
EXTEND = (
 '<h2 id="extend">How these tests extend the Pharo CI</h2>'
 '<h3>What the Pharo CI runs</h3><ul>'
 f'<li>The CI of <code>pharo-project/pharo</code> builds the image and runs all its SUnit tests (<code>test \'.*\'</code>) with the stock VM '
 f'on Linux, macOS and Windows, 64-bit, and the command-line tests (<a href="{GH}/pharo/blob/Pharo14/Jenkinsfile">Jenkinsfile</a>, '
 f'<a href="{GH}/pharo/blob/Pharo14/bootstrap/scripts/runTests.sh">runTests.sh</a>).</li>'
 f'<li>The CI of <code>pharo-project/pharo-vm</code> runs the VM unit tests (<code>VMMakerTests</code>, <code>Slang-Tests</code>), then the SUnit '
 f'tests of the image on the new VM.  A failure of the image tests marks the build, but does not stop it '
 f'(<a href="{GH}/pharo-vm/blob/pharo-12/Jenkinsfile">Jenkinsfile</a>).</li>'
 '<li>Both set <code>PHARO_CI_TESTING_ENVIRONMENT</code>, and some tests skip when it is set (see below).</li></ul>'
 '<h3>What we add</h3><ul>'
 '<li><b>A second VM.</b>  Each test runs on stock Cog v12.0.6 and on iospharo, a VM written separately (C++, with a JIT for arm64 and x86_64).  '
 'A test that fails the same way on two VMs points at the image, the test or the package, not at one VM.  The platform is macOS arm64.</li>'
 '<li><b>Skips and expected failures are failures.</b>  A test that skips or carries the expected-failure mark is work to do here, not a pass.  '
 'The CI does not report these tests as failures.</li>'
 '<li><b>The whole SUnit suite of Pharo 14 (build 791): 39465 tests in 2160 classes</b>, each parameter set of a parametrized test '
 'counted once.  Six classes that need a display (135 tests) also run in an interactive session.  The Pharo 13 suite (28077 tests) ran too.</li>'
 '<li><b>Repetition.</b>  The full suite ran 22 times on 2026-11-17 (13 times on iospharo, 9 times on Cog with our Cog changes).  Tests that '
 'failed at random ran in loops of 600 to 3000 runs.  The loops found races in the image that one CI run does not show: the TaskIt '
 'worker pool (83 failures in 600 runs before the fix, 0 in 3000 after), the wake-ups of <code>AtomicSharedQueue</code>, and the '
 'uninstall of an object-specific MetaLink, which depends on the order of a set.</li>'
 '<li><b>Packages.</b>  200 maintained packages with tests, from the <a href="https://github.com/avwohl/soogle">Soogle</a> index of public Smalltalk '
 'code: of 26,876 Pharo repositories, 528 had a push in the last year, 318 had a <code>BaselineOf</code> and tests, and 200 were chosen '
 'for variety (a limit for each GitHub organization).  Stock Cog loads each package into Pharo 14, then the two VMs run its tests.  '
 'Rows 1 to 163 ran: 155 packages loaded, with 46,419 tests.  The package tests found defects in the image that the image suite does not '
 'cover: <code>RBNamespace&gt;&gt;renameClass:to:around:</code>, <code>Duration&gt;&gt;asDelay</code> and the equality of <code>ZnUrl</code>.</li>'
 '<li><b>Stock Cog.</b>  The runs found 11 defects in stock Cog.  9 have a change on a local branch of <code>pharo-vm</code>, built and tested; '
 '2 are open (segmentation faults with no known cause).  See the Cog section.</li>'
 '</ul>'
 '<h3>Test counts: the Pharo CI and ours</h3>'
 '<p>"Fail" is failures and errors together (and timeouts in our runs).  The Pharo CI counts are from the JUnit reports on '
 '<a href="https://ci.inria.fr/pharo-ci-jenkins2/job/Test%20pending%20pull%20request%20and%20branch%20Pipeline/job/Pharo14/791/testReport/">Pharo14 build 791</a> and '
 '<a href="https://ci.inria.fr/pharo-ci-jenkins2/job/pharo-vm/job/pharo-12/151/testReport/">pharo-vm pharo-12 build 151</a>, read on 2026-11-20.  '
 '<b>The JUnit report of the Pharo CI records a skipped test and an expected failure as PASSED</b> '
 '(for example <code>FloatTest&gt;&gt;testNaNCompare</code>, which always skips), so its skip and expected-failure columns are 0 and '
 'those tests are inside its "Pass" count.</p>'
 '<table><tr><th>Test set</th><th>Run by</th><th>Total</th><th>Pass</th><th>Fail</th><th>Skip</th><th>Expected failure</th></tr>'
 '<tr><td>Pharo 14 image, build 791</td><td>Pharo CI, each of Linux, macOS, Windows (64-bit)</td><td>39452</td><td>39452</td><td>0</td><td>0 reported</td><td>0 reported</td></tr>'
 '<tr><td>Pharo 14 image, build 791</td><td>Pharo CI, isolated loads of 17 projects (Linux)</td><td>12385</td><td>12385</td><td>0</td><td>0 reported</td><td>0 reported</td></tr>'
 '<tr><td>Pharo command line (bats)</td><td>Pharo CI (Linux)</td><td>46</td><td>44</td><td>0</td><td>2</td><td>-</td></tr>'
 '<tr><td>Pharo 14 image, build 791</td><td>ours, before the repairs (old runner: 1 parameter set per test); stock Cog and iospharo the same</td><td>28413</td><td>28193</td><td>1</td><td>170</td><td>49</td></tr>'
 '<tr><td>Pharo 14 image, build 791, with the fixes</td><td>ours, iospharo (all parameter sets; 13 tests added by the fixes)</td><td>39465</td><td>39376</td><td>1</td><td>87</td><td>1</td></tr>'
 '<tr><td>Pharo 14 image, build 791, with the fixes</td><td>ours, Cog with our Cog changes</td><td>39465</td><td>39377</td><td>0</td><td>87</td><td>1</td></tr>'
 '<tr><td>6 classes that need a display</td><td>ours, iospharo, interactive session</td><td>135</td><td>135</td><td>0</td><td>0</td><td>0</td></tr>'
 '<tr><td>Pharo 13 image (the VM CI downloads Pharo 13.0 build 732)</td><td>VM CI, each of 8 runs: Linux, Windows, macOS x86_64, macOS arm64, each with and without the FFI worker</td><td>39987</td><td>39981</td><td>6</td><td>0 reported</td><td>0 reported</td></tr>'
 '<tr><td>Pharo 13.1 image</td><td>ours, first run (old runner: 1 parameter set per test)</td><td>28077</td><td>27894</td><td>2</td><td>131</td><td>50</td></tr>'
 '<tr><td>VM unit tests (<code>VMMakerTests</code>, <code>Slang-Tests</code>)</td><td>VM CI (macOS x86_64)</td><td>9543</td><td>9543</td><td>0</td><td>0</td><td>0</td></tr>'
 '<tr><td>VM unit tests</td><td>ours: not run (iospharo is not made from VMMaker)</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td></tr>'
 '<tr><td>155 packages of the 200-package run (rows 1 to 163)</td><td>Pharo CI: not run</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td></tr>'
 '<tr><td>155 packages, last run of each row</td><td>ours, stock Cog</td><td>46419</td><td>46131</td><td>121</td><td>156</td><td>11</td></tr>'
 '<tr><td>155 packages, last run of each row</td><td>ours, iospharo</td><td>46419</td><td>46128</td><td>124</td><td>156</td><td>11</td></tr>'
 '</table>'
 '<p>The 6 failures of the VM CI on Pharo 13 are the same in each of its 8 runs: '
 '<code>MirrorPrimitivesTest&gt;&gt;testExecutingPrimitive</code>, <code>OCClassBuilderTest&gt;&gt;testCreateNormalClassWithTraitComposition</code>, '
 '<code>ReleaseTest&gt;&gt;testNoShadowedVariablesInMethods</code>, <code>StDebuggerInspectorTest&gt;&gt;testUpdateLayoutForContextsWithAssertionFailure</code>, '
 '<code>StDebuggerTest&gt;&gt;testUpdateLayoutForContextsIfAssertionFailure</code> and <code>SystemDependenciesTest&gt;&gt;testExternalUIDependencies</code>.  '
 'The last 5 are Pharo 13 test defects with a fix here (<a href="bugs/open-pharo13-suite.html">Pharo 13 page</a>).  '
 'The package counts are the last run of each row; most rows ran again after a repair of the package, so the counts are not the counts before the repairs.</p>'
 '<h3>Tests that the Pharo CI accepts as known failures</h3><ul>'
 '<li><b>220 tests of Pharo 14 build 791 do not pass, the same on the two VMs:</b> 170 skips, 49 tests with the expected-failure mark and 1 '
 'failure (<code>ZnClientTest&gt;&gt;testQueryGoogle</code>, which searches the Google result page for pharo.org).  132 of them pass with a fix '
 'of the image or of the test.  4 more pass with a fix of the image and a Cog change.  56 cannot apply on macOS arm64: fixtures, Windows, '
 '32-bit.  28 need a display.  The section "Defects in the Pharo 14 image" has a page for each group.</li>'
 '<li><b>8 tests skip when <code>PHARO_CI_TESTING_ENVIRONMENT</code> is set</b> (<code>TestCase&gt;&gt;skipOnPharoCITestingEnvironment</code>).  '
 'Our runs do not set the variable, so the tests run: '
 + ', '.join(f'<code>{c}&gt;&gt;{s}</code>{n}' for c, s, n in CI_SKIP) + '.  '
 'In our 6 saved full runs of 2026-11-17, 7 of the 8 pass every time; the TaskIt test failed once.</li>'
 f'<li><b>The VM CI removes <code>ReflectivityControlTest&gt;&gt;testAfterSequence</code> before its run</b>, because the test stopped the VM '
 f'(<a href="{GH}/pharo-vm/blob/pharo-12/scripts/patchPharoPreTests.st">patchPharoPreTests.st</a>, '
 f'<a href="{GH}/pharo/issues/11745">pharo issue 11745</a>).  The test passes in our 6 saved full runs, on iospharo and on Cog with our Cog changes.</li>'
 '</ul>')

KINDS = [('image', 'Defects in the Pharo 14 image and its tests',
          'The 220 Pharo 14 tests that did not pass on the two VMs on 2026-11-17 (170 skips, 49 expected failures, 1 failure), and the defects that the sweeps found later.  One page per group of the repair record.'),
         ('open', 'Open: tests that fail at random, and Pharo 13', 'Tests with no proven cause or no fix yet.'),
         ('cog', 'Defects in the stock Cog VM', 'Changes for pharo-project/pharo-vm, made and tested on local branches.  None was sent.'),
         ('pkg', 'Defects in the packages of the 200-package run', 'Rows 1 to 163 of the package run: one page per section of the package record, and one page per open row with no section.')]
cnt = collections.Counter(p['kind'] for p in PAGES)
def cat(s):
    if s.startswith('Open'): return 'open'
    if s.startswith('Not applicable'): return 'na'
    if s.startswith('Fixed') and not any(w in s for w in ('needs', 'not run', 'only in', 'not accepted')): return 'fixed'
    return 'part'
cc = collections.Counter(cat(p['status']) for p in PAGES)
st_open, st_fixed, st_na, st_wait = cc['open'], cc['fixed'], cc['na'], cc['part']
assert st_open + st_fixed + st_na + st_wait == len(PAGES)
body = [f'<h1>Pharo test defects</h1>' + EXTEND + f'<h2 id="defects">The defects</h2><p class="lede">Each test that did not pass on the iospharo VM and on the stock Cog VM, '
        f'grouped by the defect behind it.  The defects are in the Pharo 14 image, in its tests, in the stock Cog VM, and in packages '
        f'that load into Pharo 14.  Each page has a summary, a test that shows the defect, and the fix when one is known.  '
        f'The fixes are proposals for review: none was sent to a Pharo repository yet.</p>',
        '<div class="counts">' + f'<div><b>{len(PAGES)}</b>pages</div><div><b>{st_fixed}</b>fixed</div><div><b>{st_open}</b>open</div><div><b>{st_na}</b>not applicable</div><div><b>{st_wait}</b>partly fixed, or the fix needs a Cog change or a decision</div>' + '</div>',
        '<p>How the results were made: each test ran on two VMs, the stock Cog VM and iospharo, a second Pharo VM (a C++ reimplementation with a JIT for arm64 and x86_64).  '
        'A test that fails the same way on two VMs points at the image, the test or the package, not at one VM.  '
        'In the records, <i>ours</i> and <i>our VM</i> mean iospharo.  <i>Stock Cog</i> is the Pharo VM v12.0.6 (arm64), macOS arm64.  '
        '<i>Patched Cog</i> is stock Cog with the Cog changes of the Cog section.  The image is Pharo 14, build 791.  '
        'A <i>sweep</i> runs the full test suite of the image (39465 tests).  The full list of tests is in '
        + f'<a href="files/nonpassing-tests-2026-11-18.tsv">nonpassing-tests-2026-11-18.tsv</a>.</p>',
        '<p>The 220 Pharo 14 tests of 2026-11-17: 132 fixed by an image fix, 4 fixed in the image but stock Cog also needs a Cog change, '
        '56 cannot apply on macOS arm64 (12 of them with an open point), 28 need an interactive session.  18 more tests came up during the work, '
        'and 7 tests failed at times in earlier runs.</p>',
        '<p>About the Upstream column: the Pharo 14 image is built from <code>pharo-project/pharo</code> and from projects that '
        '<code>BaselineOfPharo</code> loads at a fixed version: Spec v2.0.6, NewTools v0.11.0, NewTools-DocumentBrowser v1.1.1, Microdown v2.11.0, '
        'Roassal v1.7.7 (which loads Geometry v1.2.1).  A fix in such a project gets into the image when Pharo moves the version.  '
        'The development branch is <code>Pharo15</code> (Microdown: <code>dev</code>, Geometry: <code>master</code>); the fixes were measured on Pharo 14 build 791.</p>']
for kind, title, text in KINDS:
    ps = [p for p in PAGES if p['kind'] == kind]
    body.append(f'<h2 id="{kind}">{h(title)} ({len(ps)})</h2><p>{h(text)}</p>')
    trs = ['<tr><th>Defect</th><th>State</th><th>Tests</th><th>Fix</th><th>Upstream</th></tr>']
    for p in ps:
        trs.append(f'<tr><td><a href="bugs/{p["slug"]}.html">{h(p["title"])}</a></td><td>{status_badge(p["status"])}</td>'
                   f'<td>{p["tests"] or "-"}</td><td>{h(p["fix"].replace("image fix ", "")[:60])}</td><td>{h(p["upstream"] or "-")}</td></tr>')
    body.append('<table>' + ''.join(trs) + '</table>')
open(OUT + '/index.html', 'w').write(page('Pharo test defects', ''.join(body), 0,
     'Each test that did not pass on the iospharo VM and on stock Cog, with a summary, a test and a fix for each defect.').replace('<p class="crumb"><a href="index.html">Pharo test defects</a></p>', ''))
json.dump(PAGES, open(OUT + '/../pages.json', 'w'), indent=1)
print(len(PAGES), 'pages', dict(cnt), 'files', len(FILES)); print('excluded:', EXCLUDED)
