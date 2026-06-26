#!/usr/bin/env python3
"""Pick a diverse, ranked candidate pool from enriched.tsv for verification."""
import re, csv
from collections import defaultdict

ORG_CAP = 14
POOL = 240
IMAGE_FORK = re.compile(r'^pharo\d+$|^pharo$|bootstrap', re.I)

rows = []
with open("enriched.tsv") as f:
    r = csv.DictReader(f, delimiter="\t")
    for d in r:
        if d["loadable"] != "1":
            continue
        if IMAGE_FORK.search(d["repo"]) or d["baseline"] == "PharoBootstrapProcess":
            continue
        d["n_tests"] = int(d["n_tests"] or 0)
        d["stars"] = int(d["stars"] or 0)
        rows.append(d)

# rank: more test classes first, then stars
rows.sort(key=lambda d: (-d["n_tests"], -d["stars"]))

# per-org diversity cap
per_org = defaultdict(int)
kept = []
for d in rows:
    if per_org[d["owner"]] >= ORG_CAP:
        continue
    per_org[d["owner"]] += 1
    kept.append(d)
    if len(kept) >= POOL:
        break

def ghpath(d):
    p = "github://%s/%s:%s" % (d["owner"], d["repo"], d["branch"])
    return p + ("/" + d["srcdir"] if d["srcdir"] else "")

cols = ["owner", "repo", "branch", "baseline", "srcdir", "n_tests", "stars",
        "ghpath", "topics", "desc"]
with open("selected.tsv", "w") as f:
    f.write("\t".join(cols) + "\n")
    for d in kept:
        d["ghpath"] = ghpath(d)
        f.write("\t".join(str(d.get(c, "")).replace("\t", " ") for c in cols) + "\n")

print("pool: %d packages, %d orgs" % (len(kept), len(per_org)))
print("n_tests: >=20:%d  >=10:%d  >=5:%d  >=2:%d  <2:%d" % (
    sum(d["n_tests"] >= 20 for d in kept), sum(d["n_tests"] >= 10 for d in kept),
    sum(d["n_tests"] >= 5 for d in kept), sum(d["n_tests"] >= 2 for d in kept),
    sum(d["n_tests"] < 2 for d in kept)))
print("top 12:", ", ".join("%s/%s(%d)" % (d["owner"], d["repo"], d["n_tests"]) for d in kept[:12]))
