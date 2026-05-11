# JIT-vs-interp differential fuzzer

Tiny harness for finding JIT bugs by running the same Pharo expression
under `PHARO_NO_JIT=1` (interpreter, ground truth) and under JIT-default,
then comparing.

## Files

- `corpus.txt` — one test per line: `<id>|<expression>|<expected-pattern>`.
  Pattern is an egrep regex the OUTPUT must match; if empty, only
  interp-vs-JIT equivalence is checked.
- `run.sh` — runner.  Writes `results/report.md` (markdown table),
  `results/results.csv` (machine-readable), and per-test
  `results/<id>_{interp,jit}.{out,err}` files for each FAILURE.
- `bisect.sh <test_id>` — given a failing test, finds the minimal
  `JIT_EXCLUDE` selector list that makes it pass.  Helps localize.
- `results/` — generated.  Cleared on each `run.sh` run for failures
  (passes leave no artifacts).

## Usage

```bash
# build VM first
PHARO_DISABLE_LTO=1 bash scripts/build-linux.sh

# run full corpus (defaults: 30s/test timeout, /tmp/Pharo.image)
bash scripts/jit-diff/run.sh

# faster iteration
PHARO_DIFF_TIMEOUT=15 bash scripts/jit-diff/run.sh

# bisect a single failing test
bash scripts/jit-diff/bisect.sh arith_001
```

## Interpreting status codes

- `PASS` — interp and JIT produce the same answer (and match the
  expected pattern, if one was given).
- `JIT_DIFF` — JIT differs from interp.  This is the bug-finding
  signal.  Both `_interp.out` and `_jit.out` are kept for inspection.
- `JIT_TIMEOUT` — JIT hung past the timeout.  Often a JIT bug that
  loops (e.g. the `findNextHandlerContext` privHandlerContext DNU
  loop documented in `memory/jit_temp_corruption_hunt.md`).
- `INTERP_TIMEOUT` — interp itself hung.  Indicates a bad test (the
  expression is genuinely slow), not a JIT bug.
- `INTERP_BAD` — interp ran but produced output that didn't match the
  expected pattern.  Either a typo in the corpus (`\\` escaping etc.)
  or the test depends on something this image doesn't have.

## When JIT is fixed

The goal is to grow the corpus and watch `JIT_DIFF` and `JIT_TIMEOUT`
counts trend toward zero.  Add new tests for any JIT bug a debugging
session uncovers — that locks in the regression coverage.

## Limitations

- One eval per test = one image-load = ~5–10s overhead.  Fine for a
  100-test corpus; impractical at 10k.  If the corpus grows that
  large, switch to a long-lived VM that accepts expressions over a
  pipe.
- Result extraction matches the LAST non-blank line between
  `Image args:` and `=== Execution Summary ===`.  Multi-line outputs
  get truncated.
- `bisect.sh` runs the test ~N² times in the worst case (N =
  candidate selector count).  Bound the corpus carefully, or run
  in parallel.
