# iOS Pharo VM — Work In Progress

## Current Status (2026-02-19)

### Test Results — Full Run (commits up to d337e7e)

Previous run (commit 4c056bd): ~12262 pass, 52 fail, 43 error, 20 skip, ~132 timeout
Current run (in progress, 254/576 classes): significant improvements

### Key Fixes This Session

1. **ProcessTerminateBugTest** (commit 1fceb0a): NLR reachability check in `returnValue()`.
   When `Context>>jump` transfers control between stacks (e.g., during process termination
   via `runUntilReturnFrom:`), pending NLR state pointed to the terminated process's stack.
   Fix: walk sender chain to verify NLR target is reachable before continuing NLR.
   Result: 12/12 pass (was 10/12).

2. **Test runner execution environment** (commit 86e2b07): Wrapped test batch in
   `CurrentExecutionEnvironment runTestsBy:` which activates `TestExecutionEnvironment`.
   This provides `processMonitor` etc. that ProcessTest needs.
   Result: 46 ProcessTest errors → 0.

3. **Float write immutability** (commit 86e2b07): Removed immutability checks from
   primitives 628/629 (float32/float64 write). Reference VM writes through immutability;
   the Smalltalk tests are marked `<expectedFailure>`.
   Result: 2 WriteBarrierTest errors → 0.

4. **primitiveContextSize** (commit d337e7e): Was returning allocated capacity
   (`slotCount - 6`) instead of current `stackp` value. Object>>halt has capacity 16
   but stackp=1.
   Result: InstructionStreamTest `testSimulatingAMethodWithHaltHasCorrectContext` fixed.

5. **Performance optimizations** (uncommitted):
   - `lookupInMethodDict`: Rewrote from O(n) linear scan with `std::string` heap allocation
     to O(1) hash-based probe with identity comparison. Symbols are interned.
   - `methodClassOf`: Replaced 6 string comparisons + `std::string::find` with simple
     slot count check (reference VM approach).
   - Timer check interval: 100 → 1024 steps (reduces syscall overhead 10x).
   These fix the ~1000x slowdown for class hierarchy operations that caused most timeouts.

### Remaining Issues

| Category | Count | Status |
|---|---|---|
| Timeouts | ~80 | Mostly performance-related; perf fixes should help dramatically |
| ProcessTest errors | ~46 | Fixed (execution environment) |
| WriteBarrierTest errors | 2 | Fixed (float write immutability) |
| ProcessTerminateBugTest | 2 | Fixed (NLR reachability) |
| OpalCompiler AST cache | ~21 | Array basicNew: failure cascade — test contamination |
| Slot/class layout tests | ~5 | Test isolation issues — previous tests corrupt class hierarchy |
| SHA1/MD5 hash tests | 3 | Missing DSAPrims plugin — image code has Character>>bitShift: bug |
| WeakKeyDictionary | 2 | GC finalization timing — weak entries cleared too early |
| CodeSimulationTest | 1 | prepareMethod:forSimulationWith: not impl for SistaV1 |
| testCannotBeRecompiled | 1 | Fails on reference VM too (Pharo image bug) |
| testNoWeakBlock | 1 | Fails on reference VM too (no ephemeron support for weak blocks) |

### Delay Scheduler Health

No `DELAY-DEAD` messages in current run (254+ classes). The per-process NLR state
fix from the previous session appears to have resolved the Delay scheduler death issue.

### GUI Status

- Desktop renders correctly (Pharo world with morphs)
- Top menu bar visible and clickable, dropdowns open
- World menu opens on right-click
- Dragging startup window makes it disappear (window management issue)
- Menu actions don't execute (likely event handling / morphic issue)

### Architecture

- `src/vm/Interpreter.cpp` — Sista V1 bytecode interpreter
- `src/vm/Primitives.cpp` — Primitive implementations
- `src/vm/ObjectMemory.cpp` — Memory management, GC
- `scripts/run_sunit_tests.st` — Test runner (chunk format)
- `docs/SistaV1-Bytecode-Spec.md` — Bytecode reference
