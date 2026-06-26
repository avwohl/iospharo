#!/usr/bin/env python3
"""Assemble the final 200-package manifest from the verification workflow output."""
import json, re, sys, csv
from collections import defaultdict, Counter

OUT = "/private/tmp/claude-501/-Users-wohl-src-iospharo/e873ce7f-b5a3-4455-b221-36bedc9d873c/tasks/wo4omn0yq.output"
SEL = "selected.tsv"

recs = json.load(open(OUT))["result"]["records"]
# de-dup by (owner,repo), last wins
by = {}
for r in recs:
    by[(r["owner"], r["repo"])] = r

# join signals from selected.tsv
sig = {}
with open(SEL) as f:
    for d in csv.DictReader(f, delimiter="\t"):
        sig[(d["owner"], d["repo"])] = d
for k, r in by.items():
    s = sig.get(k, {})
    r["n_tests"] = int(s.get("n_tests", 0) or 0)
    r["stars"] = int(s.get("stars", 0) or 0)
    r["topics"] = s.get("topics", "")

records = list(by.values())
print("total records:", len(records))
print("recommend:", Counter(r["recommend"] for r in records))
print("headless_risk:", Counter(r["headless_risk"] for r in records))
print("category:", Counter(r["category"] for r in records))
print("p13:", Counter(r["p13"] for r in records))
inc = [r for r in records if r["recommend"] == "include" and r["p13"] != "no"]
print("\nINCLUDE & p13!=no:", len(inc))
print("  by risk:", Counter(r["headless_risk"] for r in inc))
print("  by category:", Counter(r["category"] for r in inc))
print("  low+med risk:", sum(r["headless_risk"] in ("low", "med") for r in inc))
print("  visual (any risk):", sum(r["category"] == "visual" for r in inc))

if len(sys.argv) > 1 and sys.argv[1] == "--write":
    RISK = {"low": 0, "med": 1, "high": 2}
    def label(r):
        return re.sub(r"[^a-z0-9]+", "-", (r["owner"] + "-" + r["repo"]).lower()).strip("-")
    # Pool = everything still plausibly P13-loadable (drop only p13=='no').  All
    # are maintained + have a BaselineOf and tests; risk/category is METADATA for
    # which test path to use, not a disqualifier (the harness empirically loads).
    pool = [r for r in records if r["p13"] != "no"]
    # test_path: visual -> pharo-headless-test fake-GUI; else headless SUnit.
    for r in pool:
        r["test_path"] = "gui" if r["category"] == "visual" else "sunit"
        r["label"] = label(r)
    # Rank reliable-first (low/med headless risk, more tests), but the risk-tiered
    # sort still pulls in the best visual/high-risk packages (Spec/Roassal/Bloc...)
    # for the GUI path.  Org cap keeps it diverse.
    pool.sort(key=lambda r: (RISK[r["headless_risk"]], -r["n_tests"], -r["stars"]))
    per_org = defaultdict(int)
    ORG_CAP = 12
    chosen = []
    for r in pool:
        if len(chosen) >= 200:
            break
        if per_org[r["owner"]] >= ORG_CAP:
            continue
        per_org[r["owner"]] += 1
        chosen.append(r)
    if len(chosen) < 200:  # relax org cap to reach 200
        have = {id(r) for r in chosen}
        for r in pool:
            if len(chosen) >= 200:
                break
            if id(r) not in have:
                chosen.append(r)
    cols = ["label", "owner", "repo", "branch", "baseline", "test_path", "category",
            "headless_risk", "p13", "n_tests", "stars", "test_prefix",
            "jit_value", "risk_reason", "load_expr"]
    with open("packages-200.tsv", "w") as f:
        f.write("\t".join(cols) + "\n")
        for r in chosen:
            f.write("\t".join(str(r.get(c, "")).replace("\t", " ").replace("\n", " ")
                              for c in cols) + "\n")
    print("\nWROTE packages-200.tsv:", len(chosen),
          "| risk", dict(Counter(r["headless_risk"] for r in chosen)),
          "| path", dict(Counter(r["test_path"] for r in chosen)),
          "| orgs", len({r["owner"] for r in chosen}),
          "| total tests(classes)", sum(r["n_tests"] for r in chosen))
