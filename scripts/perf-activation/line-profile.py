#!/usr/bin/env python3
"""Line-level self-time profile from a macOS `sample` report.

    line-profile.py <sample-report> <binary-or-dSYM> [thread-substring] [N]

`sample` prints self time per FUNCTION ("Sort by top of stack") but not per
line.  Its call graph does carry the leaf addresses, so this script walks the
graph, takes each node's SELF count (its count minus its direct children's),
spreads it over the node's addresses, and asks atos for file:line.

The binary needs line tables.  build-rel has none (-O3, no -g), and with
ThinLTO the debug info lives in temporary objects the linker deletes, so a
plain -g build still symbolizes to function+offset only.  What works:

    cmake -S . -B build-prof -G Ninja -DCMAKE_BUILD_TYPE=Release \\
        -DCMAKE_CXX_FLAGS=-g -DCMAKE_C_FLAGS=-g \\
        -DCMAKE_EXE_LINKER_FLAGS="-Wl,-object_path_lto,$PWD/build-prof/lto-objs"
    cmake --build build-prof --target test_load_image
    dsymutil build-prof/test_load_image -o build-prof/test_load_image.dSYM

Only the busiest thread is used unless a thread substring is given.
"""
import collections
import re
import subprocess
import sys

report, binary = sys.argv[1], sys.argv[2]
want_thread = sys.argv[3] if len(sys.argv) > 3 else None
top_n = int(sys.argv[4]) if len(sys.argv) > 4 else 50

text = open(report).read()
load = re.search(r'\n\s+(0x[0-9a-f]+) -\s+0x[0-9a-f]+ \+?test_load_image', text).group(1)
graph = text.split('Call graph:')[1].split('Total number in stack')[0].split('\n')

node_re = re.compile(r'^([ +!:|]*)(\d+) (.+?)\s+\(in ([^)]+)\)[^\[]*(?:\[([0-9a-fx,]+)\])?')
nodes = []          # (depth, count, func, image, addrs)
for line in graph:
    m = node_re.match(line)
    if m:
        nodes.append((len(m.group(1)), int(m.group(2)), m.group(3), m.group(4),
                      (m.group(5) or '').split(',') if m.group(5) else []))
        continue
    t = re.match(r'^\s*(\d+) (Thread_\S+.*)$', line)
    if t:
        nodes.append((-1, int(t.group(1)), t.group(2), 'THREAD', []))

# Split into threads, pick one.
threads, cur = [], None
for n in nodes:
    if n[0] == -1:
        cur = [n]
        threads.append(cur)
    elif cur is not None:
        cur.append(n)
if want_thread:
    threads = [t for t in threads if want_thread in t[0][2]]
thread = max(threads, key=lambda t: t[0][1])
total = thread[0][1]

self_by_addr = collections.Counter()
self_by_func = collections.Counter()
body = thread[1:]
for i, (d, count, func, image, addrs) in enumerate(body):
    child = 0
    j = i + 1
    child_depth = None
    while j < len(body) and body[j][0] > d:
        if child_depth is None:
            child_depth = body[j][0]
        if body[j][0] == child_depth:
            child += body[j][1]
        j += 1
    own = count - child
    if own <= 0:
        continue
    self_by_func['%s [%s]' % (func[:80], image)] += own
    if image == 'test_load_image' and addrs:
        for a in addrs:
            self_by_addr[a] += own / len(addrs)

print('thread: %s  samples: %d  self accounted: %d'
      % (thread[0][2][:60], total, sum(self_by_func.values())))
print('\nSELF by function')
for k, v in self_by_func.most_common(25):
    print('%7.0f  %5.1f%%  %s' % (v, 100.0 * v / total, k))

addrs = [a for a, _ in self_by_addr.most_common(600)]
out = subprocess.run(['atos', '-arch', 'arm64', '-o', binary, '-l', load] + addrs,
                     capture_output=True, text=True).stdout.strip().split('\n')
by_line = collections.Counter()
for a, o in zip(addrs, out):
    m = re.search(r'\(([^()]+:\d+)\)\s*$', o)
    fn = o.split(' (in ')[0][:48]
    by_line['%s  %s' % (m.group(1) if m else '?', fn)] += self_by_addr[a]
print('\nSELF by line (test_load_image only)')
for k, v in by_line.most_common(top_n):
    print('%7.0f  %5.1f%%  %s' % (v, 100.0 * v / total, k))
