#!/usr/bin/env python3
"""Classify the 528 maintained Pharo repos: BaselineOf + src dir + test count.
Reads candidates.tsv, fetches each repo's git tree via gh, writes enriched.tsv."""
import json, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

BASELINE_RE = re.compile(r'(?:^|/)?(?:(?P<src>[^/]+)/)?BaselineOf(?P<name>[A-Za-z0-9_]+)/'
                         r'BaselineOf(?P=name)\.class\.st$')
TESTCLASS_RE = re.compile(r'(?:^|/)[A-Z][A-Za-z0-9_]*Test(?:s|Case)?\.class\.st$')
TESTDIR_RE = re.compile(r'(?:^|/)[A-Za-z0-9_.-]*-?Tests?(?:/|$)')
# repos that are the image / VM / meta-distribution, not a loadable package
EXCLUDE_NAMES = {'pharo', 'pharo-vm', 'opensmalltalk-vm', 'pharo-build-scripts',
                 'gtoolkit', 'Moose', 'pharo-launcher', 'pharo-local',
                 'PharoChipDesigner'}

def gh_tree(owner, repo, ref):
    try:
        p = subprocess.run(
            ["gh", "api", f"repos/{owner}/{repo}/git/trees/{ref}?recursive=1"],
            capture_output=True, text=True, timeout=45)
        if p.returncode != 0:
            return None, p.stderr.strip()[:80]
        return json.loads(p.stdout), None
    except Exception as e:                       # noqa: BLE001
        return None, str(e)[:80]

def classify(row):
    clone, branch, stars, pushed, size_kb, lic, name, desc, topics = (row + [""] * 9)[:9]
    m = re.search(r'github\.com/([^/]+)/([^/]+?)(?:\.git)?$', clone)
    if not m:
        return None
    owner, repo = m.group(1), m.group(2)
    refs = [r for r in [branch, "main", "master"] if r] or ["main", "master"]
    tree, err, used = None, None, None
    seen = set()
    for ref in refs:
        if ref in seen:
            continue
        seen.add(ref)
        tree, err = gh_tree(owner, repo, ref)
        if tree is not None:
            used = ref
            break
    out = dict(owner=owner, repo=repo, branch=used or branch, stars=stars, pushed=pushed,
               size_kb=size_kb, license=lic, baseline="", srcdir="", n_tests=0,
               loadable=0, reason="", topics=topics, desc=desc)
    if tree is None:
        out["reason"] = "tree-fetch-failed:%s" % (err or "?")
        return out
    paths = [t["path"] for t in tree.get("tree", []) if t.get("type") == "blob"]
    truncated = tree.get("truncated", False)
    # baselines: prefer src dir in (src, repository, source) and name == repo
    baselines = []
    for pth in paths:
        bm = BASELINE_RE.search(pth)
        if bm:
            baselines.append((bm.group("src") or "", bm.group("name")))
    def brank(b):
        src, nm = b
        return (0 if src in ("src", "repository", "source", "") else 1,
                0 if nm.lower() == repo.lower().replace("-", "") else 1,
                len(src))
    n_tests = sum(1 for pth in paths if TESTCLASS_RE.search(pth))
    has_testdir = any(TESTDIR_RE.search(pth) for pth in paths)
    out["n_tests"] = n_tests
    if baselines:
        src, nm = sorted(baselines, key=brank)[0]
        out["baseline"], out["srcdir"] = nm, src
    reasons = []
    if repo in EXCLUDE_NAMES or name in EXCLUDE_NAMES:
        reasons.append("image/vm/meta")
    if truncated:
        reasons.append("tree-truncated(huge)")
    if not baselines:
        reasons.append("no-baseline")
    if n_tests == 0 and not has_testdir:
        reasons.append("no-tests")
    out["reason"] = ",".join(reasons)
    out["loadable"] = 1 if (baselines and (n_tests > 0 or has_testdir)
                            and not reasons) else 0
    return out

def main():
    rows = []
    with open("candidates.tsv") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if parts and parts[0]:
                rows.append(parts)
    results = []
    with ThreadPoolExecutor(max_workers=12) as ex:
        for i, r in enumerate(ex.map(classify, rows)):
            if r:
                results.append(r)
            if (i + 1) % 50 == 0:
                sys.stderr.write("...%d/%d\n" % (i + 1, len(rows)))
    cols = ["owner", "repo", "branch", "stars", "pushed", "size_kb", "license",
            "baseline", "srcdir", "n_tests", "loadable", "reason", "topics", "desc"]
    with open("enriched.tsv", "w") as f:
        f.write("\t".join(cols) + "\n")
        for r in results:
            f.write("\t".join(str(r.get(c, "")).replace("\t", " ") for c in cols) + "\n")
    load = [r for r in results if r["loadable"]]
    sys.stderr.write("\nDONE: %d classified, %d loadable (baseline+tests)\n"
                     % (len(results), len(load)))

if __name__ == "__main__":
    main()
