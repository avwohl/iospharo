#!/usr/bin/env python3
import csv
from collections import Counter

rows = list(csv.DictReader(open("packages-200.tsv"), delimiter="\t"))
for r in rows:
    r["n_tests"] = int(r["n_tests"] or 0)
path = Counter(r["test_path"] for r in rows)
risk = Counter(r["headless_risk"] for r in rows)
p13 = Counter(r["p13"] for r in rows)
total_tests = sum(r["n_tests"] for r in rows)
orgs = len({r["owner"] for r in rows})

def tbl(subset):
    out = ["| # | package | tests | risk | P13 | JIT value |",
           "|---|---------|------:|------|-----|-----------|"]
    for i, r in enumerate(sorted(subset, key=lambda r: -r["n_tests"]), 1):
        jv = (r["jit_value"] or "")[:60]
        out.append("| %d | `%s/%s` | %d | %s | %s | %s |" %
                   (i, r["owner"], r["repo"], r["n_tests"], r["headless_risk"], r["p13"], jv))
    return "\n".join(out)

sunit = [r for r in rows if r["test_path"] == "sunit"]
gui = [r for r in rows if r["test_path"] == "gui"]

doc = f"""# 200 maintained Pharo packages for JIT correctness + speed testing

The kernel SUnit suite passes at Cog parity but under-covers real-world bytecode.
This is the **200-package expansion** of the [package-testing harness](jit-test-packages.md):
a curated set of maintained, test-bearing Pharo packages mined from the
[soogle](https://github.com/avwohl/soogle) index, to load and run on **both** the
custom JIT VM and stock Cog so any pass-rate divergence surfaces a JIT/VM bug.

The machine-readable manifest is **`scripts/pkg-jit-test/packages-200.tsv`**
(columns incl. the exact Metacello `load_expr`, the SUnit `test_prefix`, the test
path, and a headless-load risk). Run them with
**`scripts/pkg-jit-test/run-manifest.sh`** (best on the AWS build box — the
keep-alive lease now keeps that box up while a Claude drives the sweep).

## How the 200 were chosen (the funnel)

| stage | count | filter |
|-------|------:|--------|
| Pharo packages in soogle | 26,876 | `dialect=pharo` |
| maintained | 528 | pushed in the last year, non-fork, non-archived, GitHub |
| have a BaselineOf **and** tests | 318 | git-tree scan for `BaselineOf*` + `*Test*.class.st` |
| diverse ranked pool | 240 | per-org cap, image/VM forks dropped, ≥2 test classes |
| **final selection** | **200** | per-package verified load expr + P13 branch + risk class; dropped only P13-incompatible |

Every one of the 200 was checked against GitHub: its chosen branch resolves and
its `BaselineOf` file exists on that branch.

## Breakdown

- **Test path:** {path['sunit']} headless **SUnit**, {gui and path['gui']} **visual**
  (run via the [`pharo-headless-test`](https://github.com/avwohl/pharo-headless-test)
  fake-GUI prelude — Spec/Bloc/Roassal/Morphic/menu/graphics packages).
- **Headless-load risk:** {risk['low']} low / {risk['med']} med / {risk['high']} high
  (risk = chance it won't Metacello-load headless into P13; the harness records
  actual load success — risk is a hint, not a gate).
- **Pharo 13:** {p13.get('yes',0)} yes / {p13.get('maybe',0)} maybe.
- **{orgs} distinct GitHub orgs**, **{total_tests} test classes** total.
- Coverage themes (JIT value): recursive-descent **parsing**, **serialization**
  /reflection, **numeric/float** (PolyMath/DataFrame/AI), **collection/hash**,
  large **object graphs** (Famix/Moose metamodels), **2D geometry** (graphics).

## Running the sweep

```sh
# on the box (16 vCPU), after a stock Pharo 13.1 + launcher is in place:
PHARO=/tmp/h3/pharo BASE_IMAGE=/tmp/h3/Pharo.image \\
  CUSTOM_VM=$PWD/build-rel/test_load_image \\
  scripts/pkg-jit-test/run-manifest.sh            # resumable; writes /tmp/pkg200/summary.tsv
```
Per package it: copies a clean image → stock-Cog Metacello load+save → runs the
suite on both VMs → records load status + the JIT-only failure count (candidate
regressions). Visual packages get the fake-GUI prelude automatically.

## The packages — headless SUnit ({path['sunit']})

{tbl(sunit)}

## The packages — visual / pharo-headless-test ({path['gui']})

{tbl(gui)}
"""
open("jit-test-packages-200.md", "w").write(doc)
print("wrote jit-test-packages-200.md (%d sunit + %d gui = %d)" % (len(sunit), len(gui), len(rows)))
