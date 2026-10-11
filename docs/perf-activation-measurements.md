# perf-activation: measurements on real workloads

Moved from `scripts/perf-activation/README.md`, which holds the harness and
the microbenchmark baseline.  This file holds the measurements taken on
real workloads: defect #6's whole-image scan and the printString loop.

## 2026-09-04: a REAL-workload profile and knob triage (not the microbench)

Everything above is `bench_activation.st`.  This section is the same question
asked of the workload that actually costs test results — the whole-image
reflective scan behind defect #6's TIMEOUTs
(`NoUnusedVariablesLeftTest>>testNoUnusedTemporaryVariablesLeft`, run outside
SUnit so it completes rather than being killed).  Native arm64, quiet machine.

Baseline, interleaved reps:

    stock Cog (x86_64 Rosetta)     5.76 s
    ours, JIT on                 216.8 / 217.3 / 218.2 s
    ours, JIT off                177.4 / 160.9 / 161.2 s

**The JIT is 35% slower than our own interpreter here.**  That is consistent
with `docs/results-perfdb.md`'s "1M blocks: JIT 5.5x SLOWER" — a reflective
scan is `allMethodsDo: [...]`, i.e. block-activation-bound.

`sample`, 10 s mid-scan, top of stack (JIT-emitted code carries no symbols so
it is excluded; these are the C++ frames):

    724  Interpreter::tryJITResumeInCaller
    560  Interpreter::activateBlock
    443  Interpreter::tryJITActivation
    436  ObjectMemory::scavenge()::$_3
    386  JITRuntime::noteMethodEntry
    327  Interpreter::interpret
    319  JITRuntime::tryResume
    295  Interpreter::push
    254  Interpreter::upgradeICToJ2J
    196  Interpreter::primitiveFullClosureValue

Knob triage, one rep each, same expression, same machine:

    knob                          time      delta vs default
    (default)                    217.3 s      --
    PHARO_T1_NO_CALLER_RESUME    187.2 s     -14%
    PHARO_NO_BLOCK_VALUE_SPEC    216.9 s       0
    PHARO_NO_EAGER_BLOCK_VALUE   217.5 s       0
    PHARO_T1_NO_INLINE_J2J       220.8 s      +2%
    PHARO_NO_JIT                 161.0 s     -26%

**Only the caller-resume path has a measurable share**, and it is 14% — which
matches `tryJITResumeInCaller` topping the profile, and makes hypothesis 1 (the
bcToCode resume-address lookup on the J2J return path) the one with evidence
behind it.  The other 12% of the gap to the interpreter is spread across
`activateBlock` / `tryJITActivation` / `noteMethodEntry` and does not fall to
any single existing knob.

Two things NOT to re-test:

  * the block-value specialisations — `PHARO_NO_BLOCK_VALUE_SPEC` and
    `PHARO_NO_EAGER_BLOCK_VALUE` are each within noise of the default, so the
    block cost is in the generic activation path, not in those emits;
  * `noteMethodEntry`'s `hasSplice()` — it looks like the per-call
    `unordered_set` lookup CLAUDE.md warns about, but `sistaRuntimeForGCHook_`
    is null unless `PHARO_SISTA_COMPILE`/`sistaDispatch` is set, so the lookup
    never runs in a default build.

`PHARO_T1_NO_CALLER_RESUME` is NOT a fix — it is the x86 sp-leak workaround
knob, it does not close the gap to the interpreter (187 s vs 161 s), and
disabling caller re-entry costs send-bound code where the JIT does win.  It is
useful here only as an attribution instrument.


## 2026-10-11: the gap is general, and two interpreter hot-path fixes

`bench_ast_phases.st` splits defect #6's scan into phases.  Native arm64 for
ours, stock Cog x86_64 under Rosetta, ms:

    phase            Cog    ours JIT   ours interp   JIT/Cog
    enumerate         10        73          53          7x
    sourceCode        70      1849        1327         26x
    scan             114      1631        1340         14x
    parse            328      5080        3922         15x
    parse+semantic   677     12210        9186         18x
    printString 3M   552     10765        6559         20x

No phase stands out, so it is not the parser.  `1 to: N do: [:i | i
printString]` alone shows the whole problem and is the repro to use.

Per 1M printString, before -> after the two fixes below:

    interpreter (PHARO_NO_JIT)   1980 -> 1500 ms
    JIT (default)                3170 -> ~3000 ms

**Fix 1: `Interpreter::push` was the hottest interpreter frame** (863 of
~6000 samples).  Every push ran a three-region heap-bounds test plus two
large diagnostic dumps, all inline, so the function could not be inlined.
Now a one-test fast path with the old body moved byte-for-byte into a cold
`pushSlow()`.  -21% on the interpreter.

**Fix 2: `patchJITICAfterSend` was an out-of-line call per send** that
returned at once almost always (no pending patch).  Inline gate now.  -4%.

**What did NOT move the JIT number**, each 2 reps on the printString loop:

    knob                                             ms      vs ~3050
    PHARO_T1_NO_CALLER_RESUME                        2900-2934  -5%
    PHARO_NO_RESUME                                  2952-2979  -3%
    PHARO_T1_ADMIT_BAILMID_CALLEES                   3026-3028  -1%
    ADMIT_BAILMID + PHARO_T1_XMETHOD_MAX_IC=200      3977-4006  +31%

`PHARO_T1_SEND_CENSUS` on this loop: 69% of sends take the dispatchCached
round trip (top: `#byteAt:put:` 5.6 per printString, `#digitValue:` 4.9),
and the census now also prints which early return `upgradeICToJ2J` took.
Almost all of them leave at "entry matched, extras already set": the IC
already holds a J2J entry (bit 60) and the emitted send still exits
ExitSendCached.  Opening the bail-mid and numIC gates does not change the
69%, so those gates are not what refuses them.  This is the
`docs/j2j-chain-continuity.md` project (site 1); start there.

## 2026-10-11 (later): two more, now with line-level attribution

`scripts/perf-activation/line-profile.py` turns a `sample` report into self
time per source line.  Its docstring has the build recipe: with ThinLTO the
line tables live in temporary objects the linker deletes, so a plain `-g`
build symbolizes to function+offset only; `-Wl,-object_path_lto,<dir>` plus
`dsymutil` fixes that.

On the JIT printString loop it put `tryJITActivation` at 29.7% self, its two
hottest lines being:

- **`jit::SistaSave sistaSavePool[256]`** -- value-initialized because Oop's
  constexpr default constructor zeroes `receiver`: 256 stores at a 56-byte
  stride, ~14 KB of cache lines, on EVERY JIT activation, with Sista off by
  default.  Now raw uninitialized storage (nothing reads past the cursor).
  -17% on the loop (3050 -> 2540 ms); `tryJITActivation` 29.7% -> 17.3%.
- `clearJ2JSlice` (4.7%) -- kept: it wipes stale receivers the GC would
  otherwise pin, and bounding the GC walk instead regressed TFFI (2026-07-03).

**`upgradeICToJ2J` fast exit**: nearly every call finds the site's entry for
the receiver class already upgraded, but reached that return only after a
methodMap lookup, the eager-compile test, isJ2JBanned and prim
classification.  An inline scan at the top returns first.  -4%.

Phase bench after all four fixes, ms (before -> after):

    phase            JIT              interp
    parse+semantic   12210 -> 10540   9186 -> 8073
    testBody         12632 -> 11139   9733 -> 8420
    printString 3M   10765 ->  8643   6559 -> 4960

SUnit batches 1-200 and 1001-1100: same P/F/E as the full sweep, 8-20% less
wall time.
