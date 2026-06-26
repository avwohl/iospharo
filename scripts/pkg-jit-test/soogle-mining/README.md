# soogle-mining — how the 200-package manifest was built

Provenance + reproducibility for `../packages-200.tsv` (and `docs/jit-test-packages-200.md`).
The pipeline mines the [soogle](https://github.com/avwohl/soogle) Smalltalk-package
index for maintained, test-bearing Pharo packages to test the JIT against.

## Pipeline

| step | script | input → output | what it does |
|------|--------|----------------|--------------|
| 1 | (mysql) | soogle DB → `candidates.tsv` | export maintained Pharo repos: `dialect=pharo`, pushed in the last year, non-fork, non-archived, GitHub. **26,876 Pharo → 528 maintained.** |
| 2 | `detect.py` | `candidates.tsv` → `enriched.tsv` | `gh` git-tree scan of each repo: find `BaselineOf*` + count `*Test*.class.st`; classify loadable. **528 → 318 with a baseline + tests.** |
| 3 | `mk_select.py` | `enriched.tsv` → `selected.tsv` | drop image/VM forks, per-org diversity cap, rank by test count. **318 → 240 pool.** |
| 4 | (workflow) | `selected.tsv` → `verification-240.tsv` | a 30-agent verification workflow reads each `BaselineOf` + branches, picks a **Pharo-13** branch/group, writes a verified Metacello `load_expr` + `test_prefix`, classifies sunit/visual + headless-load risk. |
| 5 | `assemble.py` | verification + `selected.tsv` → `../packages-200.tsv` | join, drop P13-incompatible, risk-rank, per-org cap → **final 200** (with a `test_path` of `sunit` or `gui`). |
| 6 | `validate.py` | `../packages-200.tsv` → checks | confirm every chosen branch + `BaselineOf` file resolves on GitHub. **200/200 OK.** |
| 7 | `gen_doc.py` | `../packages-200.tsv` → `docs/jit-test-packages-200.md` | render the human-readable doc + tables. |

## Re-running

Needs: SSH access to the soogle MySQL on awohl.com (step 1), an authenticated
`gh` (steps 2/6), and the multi-agent verification workflow (step 4 — see the
session that produced `verification-240.tsv`). The soogle export query (step 1):

```sql
SELECT clone_url, COALESCE(default_branch,''), stars, DATE(source_pushed_at),
       size_kb, COALESCE(license,''), name, SUBSTRING(description,1,160), topics
FROM packages
WHERE dialect='pharo' AND is_fork=0 AND is_archived=0 AND is_active=1
  AND source_pushed_at >= (NOW() - INTERVAL 1 YEAR) AND clone_url LIKE '%github.com%'
ORDER BY stars DESC;
```

Then `python3 detect.py && python3 mk_select.py`, run the verification workflow,
`python3 assemble.py --write && python3 validate.py && python3 gen_doc.py`.

The intermediate `.tsv`s are checked in so the selection is auditable without
re-running the network steps.
