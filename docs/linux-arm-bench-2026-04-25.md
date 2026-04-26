# Linux ARM64 bench results 2026-04-25

Tested on: Ubuntu Parallels VM, ARM64, Apple M-series host, gcc 15.

## Build state

  - All ifdefs in VM core eliminated (commits up through 09f5169).
  - LTO/IPO required CMake policy CMP0069 NEW; fixed in c93f155.
    Confirmed `-- LTO/IPO: enabled` in cmake configure output.
  - SUnit OrderedCollectionTest 351/351 passes — same as Mac.

## Bench results (block(500K) × 10 iterations after 5 warmup)

```
                    JIT off   JIT on
  Mac M (native)    26 ms     22 ms
  Linux ARM         50 ms     1000 ms (!)  → JIT 25x SLOWER than interp
  Stock Cog Linux   —         1 ms
```

## Surprising finding: JIT is slower than interpreter on Linux

The exact opposite of the W^X hypothesis.  We expected RWX-without-flips
to give Linux a perf win over Mac's per-call MSR flips.  Instead JIT
makes Linux 20x SLOWER than its own interpreter.

JIT stats from a Linux JIT-on run:

```
  compiled: 651 methods
  IC: 99% hit
  activations: 89% hits
  J2J-r: 991 / 7440854    ← chain loop fires 0.01% of the time
  J2J stencil: 72/20375 calls
  7.47M total sends
```

7.47M sends in 1000ms = 135 us per send.  The chain loop hardly engages,
so most sends take the slow tryResume → C++ → entry path.  On Mac the
W^X flip dominates that path (55% per `sample`).  On Linux the W^X
flip is gone, but `tryResume` still takes ~135 us per call somehow —
much more than the W^X cost would have been.

Profiling with `perf` on the Linux box:

  80.17% pharo::jit::JITRuntime::tryResume
   1.20% pharo::ObjectMemory::storePointer
   …

`tryResume` is 80% of CPU.  No single instruction stands out — it's the
function as a whole.  The methodMap unordered_map find (visible in the
disassembly) is one candidate; per-call setup/validation overhead is
another.

## What this DOESN'T tell us

  - **Whether Linux x86_64 or non-virtualized Linux ARM is similar.**
    Apple's Hypervisor.framework runs the VM on the same Apple Silicon,
    but with extra trap-and-emulate cost that may hit our hot path
    disproportionately.  A real (non-virtualized) Linux box would be a
    cleaner test.
  - **Whether the W^X hypothesis would hold on a real Linux box.**
    We don't have a measurement on bare-metal Linux ARM yet.

## Build incompatibility found

JIT enabled on the LTO build (after the c93f155 fix) hangs at warmup —
"block bench start" prints, then nothing.  Same image works with
PHARO_NO_JIT=1 → 50ms.  Same image with the pre-LTO build →
1000ms (slow, but completes).

So LTO MAY have miscompiled the JIT runtime path.  Investigation
pending — likely a problem with the inline asm in JIT_CALL macro
interacting with whole-program optimization, or the per-thread stack
walker no longer respecting the asm clobbers.

## What's next

  1. Investigate the LTO+JIT hang on Linux — probably an asm clobber
     issue after IPO.
  2. Profile the non-LTO Linux JIT build with `perf` more deeply to see
     why tryResume is 135 us/call (vs Mac's ~5 us/call equivalent).
  3. Try Linux x86_64 (separate VM or bare-metal) to factor out
     virtualization overhead.
  4. Run the bench on bare-metal Linux ARM (Pi 5, Graviton) to confirm
     whether Parallels is the issue.

## Pragmatic conclusion

Three things landed cleanly this session:

  - `#ifdef`s eliminated from VM core (every platform-divergent path
    lives behind `pharo::platform::*`).
  - Linux build green, 351/351 SUnit passes.
  - LTO/IPO actually enabled on Linux (CMP0069 was the gotcha).

The W^X performance hypothesis didn't pan out as expected.  Linux
isn't faster than Mac on the JIT hot path under Parallels — it's
much slower.  Whether that's Parallels-specific (trap-and-emulate
overhead on per-thread MSR-equivalent operations) or fundamental to
the Linux JIT path is the open question.  Bare-metal Linux ARM
(Pi 5, Graviton) is the next data point that would clarify.

For now, the Linux port is structurally correct (no #ifdefs, builds
clean, tests pass) but doesn't beat Mac on perf.  The platform
abstraction work is done; the JIT-perf-on-Linux investigation is its
own project.

## Update 2026-04-26 — bare-metal Pi 5 + the real bottleneck

Tested on Raspberry Pi 5 (bare-metal Ubuntu 25.10, ARM64 Cortex-A76,
4 cores).  block(500K) bench results:

```
                    JIT off    JIT on        After fix
  Mac M (native)    26 ms      22-50 ms      same
  Linux ARM Pi 5    115 ms     1980 ms (!)   134 ms
  Linux ARM VM      50 ms      1000 ms       not re-tested
  Stock Cog Linux   —          1 ms          —
```

`perf` profile on Pi 5 traced 93% of `tryResume` CPU to TWO
instructions inside `findMethodByPC`: a load of `nextInZone` (28%)
and the dependent `add` for `codeStart` (64%, IP-skid from the
load-use stall).  The function was a **linear scan through the
entire JITMethod linked list** called on every JIT entry as a
defensive validation:

    JITMethod* entryMethod = codeZone_.findMethodByPC(entry);
    if (entryMethod != jm) { /* report bug */ }

The check is logically redundant — `methodMap.lookup` already
verified `jm->compiledMethodOop` matches the lookup key, and
`entry = jm->codeStart() + codeOffset` is mathematically inside
`jm` by construction.  It was added defensively for "Bug 11b layer
4" but the actual scan (~1000 methods × 5M iterations = 5 billion
link traversals) was 90% of the bench's runtime.

Mac doesn't see this in profiles because W^X flips dominate (55%
of CPU there) and the linear scan disappears in their shadow.  On
Linux W^X is free, so the scan becomes the new bottleneck.

**Fix in commit 2d2b4fa**: gate the validation behind
`PHARO_JIT_VALIDATE_ENTRY=1` so it stays available for diagnosis
but doesn't run on the hot path.  Result:

  - Pi 5: 1980ms → 134ms (**15x speedup**).
  - Pi vs Mac ratio: was ~90x slower → now ~3x slower (matches
    Cortex-A76 vs Apple M-series CPU difference).
  - 351/351 SUnit still passes on both Mac and Pi.

The W^X hypothesis was right in *spirit* — removing per-call
overhead does help on Linux — but the dominant overhead turned out
to be a redundant validation, not the W^X scheme itself.  That
validation was hidden behind W^X cost on Mac, so we didn't see it
until profiling on a platform where W^X is free.
