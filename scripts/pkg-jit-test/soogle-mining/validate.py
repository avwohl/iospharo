#!/usr/bin/env python3
"""Validate each manifest row: chosen branch (col) + BaselineOf file actually exist."""
import csv, subprocess
from concurrent.futures import ThreadPoolExecutor

srcdir = {}
for d in csv.DictReader(open("enriched.tsv"), delimiter="\t"):
    srcdir[(d["owner"], d["repo"])] = d["srcdir"]

def gh_ok(path):
    try:
        return subprocess.run(["gh", "api", path], capture_output=True,
                              text=True, timeout=25).returncode == 0
    except subprocess.TimeoutExpired:
        return False

def check(r):
    o, repo, br, bl = r["owner"], r["repo"], r["branch"].strip(), r["baseline"]
    branch_ok = gh_ok(f"repos/{o}/{repo}/branches/{br}") or \
                gh_ok(f"repos/{o}/{repo}/git/refs/tags/{br}")
    sd = srcdir.get((o, repo), "")
    bp = (sd + "/" if sd else "") + f"BaselineOf{bl}/BaselineOf{bl}.class.st"
    base_ok = gh_ok(f"repos/{o}/{repo}/contents/{bp}?ref={br}") if branch_ok else False
    return dict(label=r["label"], ref=br, branch_ok=branch_ok, base_ok=base_ok)

rows = list(csv.DictReader(open("packages-200.tsv"), delimiter="\t"))
with ThreadPoolExecutor(max_workers=12) as ex:
    res = list(ex.map(check, rows))

bad_branch = [x for x in res if not x["branch_ok"]]
bad_base = [x for x in res if x["branch_ok"] and not x["base_ok"]]
print("validated %d rows" % len(res))
print("branch resolves: %d / %d" % (sum(x["branch_ok"] for x in res), len(res)))
print("baseline file resolves (where branch ok): %d / %d" %
      (sum(x["base_ok"] for x in res), sum(x["branch_ok"] for x in res)))
for tag, bad in (("BAD BRANCH", bad_branch), ("BASELINE NOT FOUND on branch", bad_base)):
    if bad:
        print(f"\n{tag} ({len(bad)}):")
        for x in bad:
            print("  ", x["label"], "ref=", x["ref"])
