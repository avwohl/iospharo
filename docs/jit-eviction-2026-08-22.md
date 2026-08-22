# The JIT was compiling ~4% of what it should — 2026-08-22

Three defects, each hidden behind the previous one. The net effect was that
the JIT stopped compiling a few percent into any long run and never resumed,
so the SUnit sweeps this project uses to judge the JIT were mostly measuring
the interpreter.

## 1. The eviction path was unreachable

`JITCompiler::compile` has two backends. The asmjit-T1 branch is the default
(`DebugSettings.hpp`: `useAsmjitT1 = true`, opt-out only) and it returns
unconditionally. Everything below it is the legacy stencil path — including
the entire code-zone eviction implementation: live-method pinning, LRU evict,
J2J IC scrubbing, full flush. All of it written, all of it debugged, none of
it reachable.

Meanwhile the branch that DID run, `AsmjitT1.cpp`'s allocation site, called
`zone.allocate()` and on failure did:

        g_failed++; g_failedBcOther++;
        return nullptr;

No retry, no eviction. So once the zone filled, the JIT was done for the life
of the process. What that cost, from the 2026-08-19 arm64 sweep log:

        zone fills at         160M method entries, 22,057 methods compiled
        run continues to      12.4 BILLION method entries
        compiled count        frozen at 22,060
        "Incremental evict"   appears 0 times

98.7% of that sweep ran interpreted.

Fix: extract the block into `allocateWithEviction()` (`ZoneEviction.hpp`) and
call it from both paths. A re-route of existing code.

## 2. Two W^X SIGBUSes, latent because the code never ran

Both are the same fault class fixed in `JITRuntime::tryExecute` on 2026-08-19:
a store into the MAP_JIT zone while the thread is in execute mode. Apple
Silicon keeps those pages read-only in X, and the thread default is X.

  * Eviction writes `JITMethod` headers — `->pinned`, `freeMethod()`,
    `compact()` — and those headers are bump-allocated INSIDE the zone.
    `CodeZone::allocate` opens a write window only when it SUCCEEDS, so the
    whole eviction path ran unguarded.

  * `CodeZone::allocateFromFreeList` writes free-block headers (`blockSize`,
    `next`, `*bestPrev`) while splitting a block — before `allocate()`'s
    `makeWritable`. The bump path can rely on that call; the free-list path
    cannot, because it writes during the search itself. Never hit before
    because the only thing that fills the free list is eviction. Caught on
    evict #1: SIGBUS at `str w10, [x8]` storing `rest->blockSize`.

The windows must CLOSE before each retry `allocate()`. A successful
`allocate()` deliberately leaves the thread in W for the caller's emit, and a
`ScopedPatchWriteAccess` destructor firing after it would flip back to X and
SIGBUS the first code store. `UnpinAll` carries its own window because it has
to outlive both.

## 3. The LRU was not an LRU

`CodeZone::advanceEpoch()` was never called from anywhere. `epoch_` sat at 0
for the life of the process.

`codeZone_.touch()` WAS being called on every method entry — but it stamps
`lastUsedEpoch = epoch_ = 0` on everything, and `evictLRU`'s first pass keeps
only methods with `lastUsedEpoch < threshold` where `threshold` is also 0. No
unsigned value satisfies that. So pass 1 evicted nothing, ever, and eviction
always fell through to pass 2 — which walks `firstMethod_` and takes whatever
is unpinned. That is ADDRESS order, not recency. The lowest addresses hold the
earliest-compiled methods, i.e. the kernel ones that stay hot, so every round
evicted hot code that was immediately recompiled.

Compounding it, `evictTarget` was `allocSize * 2` with the comment "amortize
eviction cost". It amortizes nothing: `allocSize` is one method, so a round
freed ~1 KB and the next compile needed another round. A round is expensive —
a full native stack walk, a scan of every process's context chain, two
O(methods) LRU passes, and a J2J scrub across every IC site of every method in
the zone.

Measured on arm64 image prep (fileIn `run_sunit_tests.st` + snapshot):

        eviction wired up, untuned      76,000 evict rounds, never finished
        + epoch advance + 1/64th zone    7.7 s, rc=0, 3 rounds, 33,681 compiled

## Correctness

SUnit batch 1-50 is the documented idle-machine baseline: 772/774 PASS,
0 FAIL, 0 ERROR. It reproduces exactly on arm64 after all three fixes.

x86_64 scores 746/774 on the same batch. All 26 deltas are
`SymbolNotFoundError` on `cairo_*` FFI lookups. That is the known host gap —
`libcairo` exists only as an arm64 build on this machine — not VM behaviour.

## What this does NOT yet establish

Throughput. The point of this change is that the JIT keeps compiling, not that
any particular workload got faster; more compilation also means more eviction
work and more recompilation. No speedup is claimed here and none has been
measured. The `PHARO_CODE_ZONE_MB=192` baseline that every recorded full-suite
run uses exists precisely because the 64 MB default starved the JIT — with
eviction working, that knob's justification should be re-measured.
