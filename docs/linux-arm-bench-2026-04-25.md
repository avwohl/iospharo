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
