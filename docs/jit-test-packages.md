# JIT test packages — broader correctness + speed coverage vs Cog

The kernel SUnit suite (565 core classes) passes at Cog parity, but it is "not
that much code" and under-covers the bytecode/send patterns real applications
hit: recursive-descent parsing, serialization/reflection, deep recursion, heavy
float loops, large object graphs, FFI. This document is the result of mining the
[soogle](https://github.com/avwohl/soogle) Smalltalk-package index for
additional packages to load and test the JIT against, plus the **real
correctness and speed results** of doing so — which surfaced multiple JIT bugs
the kernel suite missed.

## How packages are loaded and tested

Our custom VM cannot do HTTPS (its SSL backend is a no-op stub), so it cannot run
Metacello network loads itself. The mechanism (which matches the existing prep
harness):

1. The **stock Cog/Pharo 13.1 VM** Metacello-loads the package and saves the
   image. (`pharo <image> eval --save "Metacello new ...; load"`)
2. **Both** the custom JIT VM and stock Cog then run the package's `TestCase`
   subclasses headless via SUnit, against the SAME saved image, and we diff
   pass-rates. The only variable is the VM, so any divergence is a VM/JIT bug.
3. Benchmark kernels run timed on both VMs for the speed track.

"Loadable" therefore means: Metacello-loads cleanly into a headless Pharo 13.1
image on the STOCK pharo binary, without native/FFI libs we don't ship, without
a display, and without a live external server at TEST time.

Fairness invariant (inherited from the kernel harness): the test-class names
live only in an EXTERNAL file (`/tmp/pkg_prefixes.txt`), never as literals in the
runner, so neither VM's literal pool biases reflective senders.

## Harness

`scripts/pkg-jit-test/`:

```
bench_spectrum.st        cross-VM speed spectrum (compute -> dispatch/alloc tax)
run_pkg_tests.st         generic SUnit runner: runs every TestCase whose package
                         name starts with a prefix in /tmp/pkg_prefixes.txt
run-pkg-jit-test.sh      driver: runs a loaded image on BOTH VMs and diffs fails
load_*.st                Metacello load scripts (run on stock pharo, eval --save)
results/                 captured benchmark logs
```

Run a package end to end (after the stock VM has loaded + saved the image):

```
scripts/pkg-jit-test/run-pkg-jit-test.sh /tmp/pkgtest/polymath.image polymath Math-Tests
```

It prints both VMs' RESULT lines and the JIT-only / Cog-only failure sets.
Always use the `-O2` `build-rel/test_load_image` for fair timing (the default
`build/` is -O0, ~9x slower).

## CORRECTNESS RESULTS (custom JIT vs stock Cog, same saved image)

```
package    classes  Cog P/F/E      JIT P/F/E/T       verdict
NeoJSON    11       116/0/0        116/0/0           CLEAN PARITY
NeoCSV     ~3       66/0/0         66/0/0            CLEAN PARITY
STON       11       317/0/0        316/0/1           1 JIT bug (deep recursion)
PolyMath   90       777/0/0        707/0/69/1        51 JIT bugs + 18 VM-core bugs
Fuel       46       733/10/5       aborted early     >=1 JIT bug halts the runner
Soil       ~30      ~425/6/2/1     SIGABRT           VM FFI file-lock crash
```

(Cog's Soil fail/err are lint + concurrency tests that fail on Cog too — i.e.
baseline, not VM bugs. Fuel's 10F/5E are WideString/symbol-global image-compat
failures that also fail on Cog.)

### Bugs surfaced (each confirmed JIT-specific by re-running the failing test
under `PHARO_NO_JIT`, which passes — unless noted)

1. **STON deep-recursion failure — VM-CORE stack limit, FIXED 2026-06-19.**
   `STONReaderTest>>testDeepStructure` serializes a 1024-level-deep nested Array.
   It failed on our VM (and IDENTICALLY under `PHARO_NO_JIT`), succeeded on Cog.
   ROOT CAUSE: the soft recursion cap `StackOverflowLimit` was **4096**, but the
   `savedFrames_` array already holds `MaxFrameDepth=65536` — so legitimate deep
   recursion that fit the array (STON's ~600-level nested write = ~4200 method+block
   frames; `with:do:`/`encodeList:`/`do:`/`separatedBy:` push ~7 frames/level) hit
   the over-aggressive 4096 cap, and the buggy `ExitStackOverflow` spill corrupted
   the operand stack (`basicNew: ByteString` PrimitiveFailed / `0x300000000`
   frames, varying signatures). FIX: raise `StackOverflowLimit` 4096 -> 56000
   (Interpreter.hpp:641), using the already-allocated capacity (no new memory;
   56000+8192 headroom < 65536). Validated: STON 316/1 -> **317/0/0**; kernel
   regression 2379 pass / 0 fail (incl. RecursionStopperTest / LocalRecursionStopper);
   NeoJSON 116/116; factorial + off-by-one + do-splice fixes intact. This is NOT
   the full root fix (truly-unbounded recursion still can't grow like Cog's stack
   pages, and the `ExitStackOverflow` spill is still buggy AT the cap) — it stops
   rejecting recursion the array can already hold. The proper fix (growable stack /
   spill JIT frames to heap contexts) remains future work.

   Bug found in the same area — ROOT-CAUSED to a SISTA tier-2 unsound type
   narrowing, and **FIXED 2026-06-19**: **`IntegerTest>>testSlowFactorial`** failed
   because `Integer>>factorial` (2-partition loop) miscompiled — ONE value per run
   came out a VALID PARTIAL PRODUCT (the accumulator `acc` lost its value at ~iter
   60; oracle = badval × a ~211-digit ratio). `slowFactorial` (plain recursion) is
   correct. ROOT CAUSE: Sista's type inference narrows the loop-carried accumulator
   `acc` to `kOopSmallInt`, so the `kPrimTagCheckInt` deopt guard is SKIPPED at
   lowering (`SistaLowering_arm64.cpp` ~730). But `acc` CAN become a LargeInteger:
   `acc * nex` overflows, deopts to the interpreter, and a later yield/re-dispatch
   re-enters the Sista-compiled loop with `acc` now Large. The skipped guard lets the
   Large value be used as a SmallInteger → corruption. CONFIRMED: the verify-on-store
   instrumentation (`PHARO_SISTA_VERIFY_STORE/LOAD`, this commit) showed exactly ONE
   Large `acc` load in 20000 calls (coinciding with the one bad result), and
   `PHARO_SISTA_NO_TAGCHECK_SKIP=1` made it 0. FIX: never skip the tag-check guard for
   a FRAME-READING operand (`kLoadTemp`/`kPhi`/`kLoadStackSlot`) — those can be
   re-entered with a type-violating value; only skip for region-local SmI values
   (constants, checked-arith results). Perf-neutral (~1-2%, within noise). Opt-out
   `PHARO_SISTA_NO_TAGCHECK_SKIP` forces the guard always. Validated: factorial repro
   1→0, hammer (177! ×20000) 1→0, `testSlowFactorial` FAIL→PASS, NeoJSON 116/116,
   PolyMath 0 SubscriptOutOfBounds (off-by-one fix intact).
   `ArrayTest>>testPrintingRecursive` — FULLY FIXED 2026-06-19 (two parts):
   (a) the 4096-overflow, fixed by the `StackOverflowLimit` raise above; and
   (b) a deep-NLR `BlockCannotReturn`, unmasked once (a) was fixed. When the
   LimitedWriteStream hits the ~50000-char limit it does a non-local return (`^`) to
   truncate, from ~14000 frames deep. ROOT CAUSE of (b): the **context-NLR
   home-context search was capped at `depth < 200`** (Interpreter.cpp ~6754 + 7
   sibling bounds in the same NLR block). The print's frames materialize to contexts,
   so the `^` takes the context-NLR path; with the home ~14000 contexts up, the 200
   cap stopped the search early -> home never found -> spurious `cannotReturn:`
   (uncatchable `BlockCannotReturn`, escapes `on: Exception do:`, aborts the batch).
   NOT generic deep-NLR (a uniform self-recursive `recurseNLR:withReturn:` stays on
   the inline stack and works to 40000) — specific to the context-NLR path. FIX:
   raise all the NLR context-search bounds 200 -> 70000 (> MaxFrameDepth=65536; still
   a finite guard against a cyclic sender chain). Validated: TPR PASS; kernel 2871
   pass / 0 fail / 0 err (incl ArrayTest, no batch abort) + a 2129-test NLR/ensure:
   re-run clean; NeoJSON 116/116; STON 317/0/0; factorial + PolyMath fixes intact.
   Repro: `scripts/pkg-jit-test/repro_cyclicprint_jit.st`.

2. **PolyMath off-by-one subscript corruption** (JIT, 51 occurrences) —
   **ROOT-CAUSED + FIXED 2026-06-19.**
   `SubscriptOutOfBounds: N in a PMVector(...)` where N is exactly one past the
   end. Confirmed JIT-specific (passes under `PHARO_NO_JIT`); determinant/matmul
   in isolation are clean, so it is a cumulative-JIT-state bug, not data-dependent.

   Root cause (found via knob-bisection + the `PHARO_SP_DEPTH_CHECK` sp-desync
   detector + reading the gate, no lldb needed): the arm64 **send-bearing
   cross-method inline-J2J gate admitted `canBailMidMethod` callees** by default
   (`AsmjitT1.cpp` ~6787 force-set the gate to admit). When such a callee/block
   bails mid-body via `ExitArithOverflow` — SmallInteger arithmetic overflowing
   to a Fraction/Float, which is pervasive in numeric code and is exactly why the
   failing data is all `PMVector(0.25 0.25)` / `((1/2)(1/2) 0 0)` — the caller's
   pending inline-J2J save is never popped, leaving the operand stack +1 word.
   `PHARO_SP_DEPTH_CHECK` shows one `tryJITActivation-exit exit=6` (ExitArith-
   Overflow), `isBlock=1`, `delta=1 words` line immediately before EVERY
   SubscriptOutOfBounds; that signature is absent under `PHARO_T1_NO_INLINE_J2J`.
   Knob bisection: `NO_INLINE_J2J` and `XMETHOD_MAX_IC=0` both eliminate it (=>
   send-bearing inline-J2J); `NO_INLINE_AT_READ` does not (=> `at:` is the
   symptom, not the cause). The V2 (arm64) `ExitArithOverflow` handler does NOT
   materialize the pending save — the existing `PHARO_T1_AO_MAT_J2J` fix is
   V1/x86-only (the naive materialize "double-handles and corrupts" on V2).

   Fix: default-EXCLUDE `canBailMidMethod` callees from the send-bearing gate
   (`AsmjitT1.cpp` + `JITRuntime.hpp`), the arm64 twin of the x86 `x86HasMidBail`
   exclusion. Opt-in `PHARO_T1_ADMIT_BAILMID_CALLEES` restores the old admit.
   Cost: +5% recursion/int, +31% collection_protocols (mid-bailing callees no
   longer inlined). The perf-preserving fix (correctly pop the save on V2
   `ExitArithOverflow`) is a documented follow-up. Verified: PolyMath 11-class
   repro 48 SubscriptOutOfBounds -> 0; kernel SUnit subset + NeoJSON unchanged.

3. **Fuel Bitmap-serialization subscript corruption** (JIT).
   `FLBinaryFileStreamBasicSerializationTest>>testBitmap` →
   `SubscriptOutOfBounds: 0 in an Array(Bitmap ...)` (off-by-one, low side). Same
   family as #2 and severe enough to abort the whole runner process, which is why
   the Fuel JIT pass-count is incomplete.

4. **PolyMath SmallFloat64 / inject:into:** (18 occurrences) — RE-CLASSIFIED to a
   SISTA do-splice bug and **FIXED 2026-06-19**. `MessageNotUnderstood:
   SmallFloat64 >> #inject:into:`. The earlier "VM-core" label was wrong — it
   reproduced under NO_JIT only because the Sista dispatch hook runs in the
   interpreter too; `PHARO_NO_SISTA` (and `PHARO_NO_SISTA_DO_SPLICE`) fix it. ROOT
   CAUSE: the Sista inject:into: do-splice inlines the block, and when the block
   CAPTURES an outer method arg/temp that it only READS (e.g.
   `coefficients inject: 0 into: [:sum :each | sum * aNumber + each]` in
   `PMPolynomial>>value:` / erf), the splice treats the read-only SCALAR copied
   value as a writable temp-VECTOR (block-body refs lowered as `kLoadTempInVec`)
   and the extra capture push desyncs the simulator stack — swapping the inject
   receiver (`coefficients`) with the captured scalar (2.0) -> `inject:into:` sent
   to a SmallFloat64. Minimal repro: `(PMPolynomial coefficients: #(1 2 3)) value:
   2.0` / `1.28 errorFunction`. FIX: default the do-splice OFF (`DebugSettings.cpp`
   `noSistaDoSplice` flipped; opt-in `PHARO_SISTA_DO_SPLICE=1`) — perf-neutral on
   the spectrum (the do:-accum path that legitimately writes a vec is also under
   this flag); plus a targeted guard in the inject path that rejects captured-var
   blocks (`PHARO_SISTA_INJECT_CAPTURE` to override) for the opt-in case. Validated:
   PolyMath SmallFloat64 18->0, NeoJSON 116/116, kernel 2862 pass, off-by-one +
   factorial fixes intact.

5. **Soil FFI file-lock SIGABRT** (VM FFI robustness gap, NOT a missing symbol —
   re-analyzed 2026-06-20). Soil-File uses uFFI→LibC (`flock`/`fcntl`/`fsync`).
   CORRECTION: these are NOT stubbed/missing — `ffi::lookupFunction` resolves them
   via `dlsym(RTLD_DEFAULT)` (`FFI.cpp:379`) to the real macOS libc, and the call
   runs through real libffi (`ffi_call` via `primitiveSameThreadCallout`,
   `Primitives.cpp:28891-29142`). The SIGABRT is UNCATCHABLE: the only guard around
   `ffi_call` is an ObjC `@try/@catch` (`ObjCExceptionGuard.m:11`) which catches
   ObjC/C++ exceptions but NOT POSIX signals, and the harness installs
   SIGSEGV/SIGBUS/SIGILL/SIGTERM handlers but deliberately not SIGABRT
   (`test_load_image.cpp:768-771`). So when a real libc callout aborts (leading
   hypothesis: a variadic-`fcntl(fd,F_SETLK,struct flock*)` declared via the fixed
   (non-variadic) `primitiveDefineFunction` path -> `ffi_cif` ABI mismatch /
   bad-pointer marshalling at `Primitives.cpp:28947-29094`; libffi can `abort()` on a
   malformed cif) the process dies uncatchably and takes the test batch with it.
   FIX DIRECTION: (a) wrap the FFI call boundary (`Primitives.cpp:29132`) in a
   `sigsetjmp`/SIGABRT guard mirroring the existing SIGSEGV recovery
   (`Interpreter.cpp:2469`) -> converts the crash into a recoverable primitive
   failure (easy-medium, general VM robustness win, not just Soil); then (b) trace
   with `PHARO_FFI_TRACE` to confirm the variadic-`fcntl`/struct-`flock` ABI path and
   fix the cif selection. NOT a JIT bug; STON+NeoCSV (no FFI) run clean.

These are exactly the classes of bug the kernel micro-suite never reached: deep
recursion, iterative numeric loops, binary serialization, and FFI.

## SPEED RESULTS — is the JIT as fast as Cog?

Spectrum benchmark (`bench_spectrum.st`), arm64, `-O2` `build-rel`, JIT default-on,
median-representative single run, lower is better:

```
workload                 Cog(ms)  JIT(ms)  slowdown  what it stresses
int_loop                 123      277      2.3x      inlined SmallInteger arithmetic  (competitive)
method_recursion_fib30   193      558      2.9x      inline-J2J cross-method sends    (competitive)
largeint_factorial       72       241      3.3x      LargeInteger arithmetic
sort                     79       423      5.4x      asSortedCollection
orderedcollection_churn  141      1041     7.4x      add/removeFirst
polymorphic_sends        1725     14526    8.4x      non-inlined #class dispatch tax
collection_protocols     50       660      13x       do:/collect:/select:/inject:into:
float_loop               218      4005     18x       Float arithmetic (no unboxing)
dictionary_ops           152      2736     18x       Dictionary at:put:/at:
allocation               53       1061     20x       object new + ivar writes
string_build             87       1947     22x       WriteStream building
block_recursion_fib28    109      3792     35x       first-class block #value: recursion
set_ops                  43       1667     39x       Set hashing/includes:
```

Real-world cross-check: a NeoJSON serialize+parse round-trip (2000 iterations) is
**27x** slower (Cog 5.7s, JIT 155s) — consistent with the spectrum, since JSON is
dominated by string building, dictionary access, float printing, and blocks.

### Interpretation

- The JIT is CORRECT but only **competitive (2–5x) on its inlinable-arithmetic
  sweet spot** (integer loops, method self-recursion, large-int). This matches
  the documented `cfibx` ~2.65x measurements.
- On the patterns that dominate real packages — **floats, first-class blocks,
  collection enumeration, hashing, allocation, string building — it is 13–39x
  slower than Cog.** These are the levers that matter for "as fast as Cog" on
  real code, and they were invisible to the kernel micro-benchmarks.
- The `-O0` vs `-O2` C++ build of `test_load_image` barely changes these numbers
  (the hot loop runs as JIT-emitted machine code, so the harness opt level is
  irrelevant) — confirming the gap is in JIT codegen quality, not the C++ host.

Highest-value speed levers implied: float unboxing (int_loop 2.3x vs float_loop
18x is the starkest gap), first-class block activation (35x), and inline
primitives for collection/hash/allocation paths.

## Ranked package list (from the soogle triage)

Tier 1 — high JIT value AND clean headless load. (★ = wired up + results above.)

```
repo                       track        tests  load
svenvc/NeoJSON ★           both         11     pure JSON parse/serialize, P13 CI, zero deps
svenvc/ston ★              both         11     recursive parse + class-tag reflection
svenvc/NeoCSV ★            both         3      char-stream scan + number parse, 100k bench
PolyMathOrg/PolyMath ★     both         78     ~900 numeric tests, P13 in CI matrix
theseion/Fuel (Pharo13) ★  correctness  47     serializer: reflection/become:/contexts
ApptiveGrid/Soil ★         both         31     btree/skiplist/serializer (FFI file layer)
smarr/SMark                both         11     framework + CL/SOM/NPB compute kernels
smarr/are-we-fast-yet      speed        1      canonical VM bench set, self-verifying
svenvc/zinc                correctness  19*    encoding + resource-meta (network-free subset)
pillar-markup/Microdown    correctness  160    parser/visitor/exporter, P13+P14 CI
```

Tier 2 — real value but a port, a GUI-package dodge, or unverified P13 load:
`j-brant/SmaCC` (dynamic parser codegen; load packages directly to dodge Spec
UI), `magritte-metamodel/magritte` (reflective dispatch; P13 unverified),
`svenvc/P3` (server-free parser/crypto subset only),
`KenDickey/Cuis-Smalltalk-Shootout-Benchmark` (manual Cuis→Pharo13 port).

Tier 3 / skip: `rakki-18/Matrix-Benchmarks` (LAPACK FFI + Roassal GUI in the test
path), `pharo-project/pharo-benchmarks` (Athens/Cairo + Morphic + FFI, 5y stale),
`VMMaker/CogBenchmarks` (no GitHub repo / no BaselineOf — unloadable).

`*` Zinc count is the network-free subset; the full repo has 65 test classes but
socket/TLS classes are excluded.

## Load expressions (run on the STOCK pharo VM with `eval --save`)

```
NeoJSON   Metacello new repository: 'github://svenvc/NeoJSON:master/repository';
            baseline: 'NeoJSON'; load: #('default' 'examples').
STON      Metacello new baseline: 'Ston';
            repository: 'github://svenvc/ston:master/repository'; load.
NeoCSV    Metacello new repository: 'github://svenvc/NeoCSV/repository';
            baseline: 'NeoCSV'; load.
PolyMath  Metacello new repository: 'github://PolyMathOrg/PolyMath:master/src';
            baseline: 'PolyMath';
            load: #('Core' 'Extensions' 'Tests' 'Benchmarks' 'Accuracy').
Fuel      "unload baked-in Fuel-* packages first, then:"
          Metacello new repository: 'github://theseion/Fuel:Pharo13/repository';
            baseline: 'Fuel'; load: 'Tests'.
Soil      Metacello new repository: 'github://ApptiveGrid/Soil:main/src';
            baseline: 'Soil'; load.
SMark     Metacello new baseline: 'SMark';
            repository: 'github://smarr/SMark:master/repository'; load.
Zinc      Metacello new repository: 'github://svenvc/zinc:master/repository';
            baseline: 'ZincHTTPComponents';
            load: #('Character-Encoding' 'Resource-Meta').
Microdown Metacello new baseline: 'Microdown';
            repository: 'github://pillar-markup/Microdown:v2.10.2/src';
            onConflict: [:e | e useIncoming]; onUpgrade: [:e | e useIncoming]; load: #('All').
```

See `scripts/pkg-jit-test/load_*.st` for runnable versions with error handling.

## Next steps

Correctness (each is a real, reproducible bug found by package testing):
- Root-cause the **off-by-one SubscriptOutOfBounds JIT bug** (PolyMath ×51 +
  Fuel testBitmap) — minimize below the test level, then `lldb` per the JIT
  debug recipe in CLAUDE.md. This is the highest-impact single fix.
- Root-cause the **STON deep-recursion operand corruption** (1024-deep) — likely
  the J2J save/resume at deep call depth; `PHARO_DET_SCHED=1` repro is stable.
- Triage the **SmallFloat64 `inject:into:` VM-core bug** separately (interpreter
  also fails; not JIT).
- Wire the **Soil FFI file-lock** path or skip Soil-File at test time.

Speed:
- Float unboxing, first-class block activation, and collection/hash/alloc inline
  primitives are the measured big levers (see the spectrum table).
- Wire **Are We Fast Yet** (SOM-transpile build) and the SMark CL-Benchmarks-Game
  kernels for a standard cross-VM speed report.

Broaden correctness:
- Tier-1 remaining: SMark, Zinc (encoding subset), Microdown. Tier-2: SmaCC
  (dynamic codegen — high JIT value).
```
