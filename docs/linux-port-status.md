# x86_64 / Linux build status

Status of building the clean C++ VM (with JIT) for **x86_64 Linux**. The arm64
JIT redesign happens on `jit`; this x86 work lives on `jit-x86` and is merged
back into `jit` deliberately (never pushed straight onto it).

## Build environment

Built on a disposable AWS EC2 **spot** instance — see `scripts/aws/` (README
there for the full runbook). Ubuntu 24.04, gcc 13 / libstdc++, 8 vCPU / 32 GiB.

    ./scripts/build-linux.sh                 # Release, JIT on, crypto off
    PHARO_DISABLE_LTO=1 ./scripts/build-linux.sh   # fast, non-LTO (default on the spot box)

Produces `build/test_load_image` (x86-64 ELF). LTO off is the spot-box default:
a ~3-5 min build finishes inside a spot lifetime; a full LTO build (~15 min)
risks reclaim mid-link.

## x86_64 build fixes (compiles + links clean as of jit-x86)

These three arm64-isms broke the x86_64/libstdc++ build; each is fixed by arch
guard or a portable equivalent (not a workaround — same behavior per arch):

    src/vm/no_getenv.hpp        the force-included `#define getenv X` poison broke
                                libstdc++'s `using ::getenv;` ("not declared in
                                '::'"). Fix: include <cstdlib>/<stdlib.h> first,
                                then poison — ban still fires on call sites.
    src/vm/ObjectMemory.cpp     readTSC() used ARM `mrs cntvct_el0`. Fix: arch
                                guard — rdtsc on x86, mrs on arm, chrono else.
    .../jit/Tier2Compiler_x86_64.cpp   bare `trace` undeclared at two trace
                                sites. Fix: alias to g_debug.t2X86Trace.

## Sista x86_64 lowering port (in progress)

`src/vm/jit/sista/SistaLowering_x86_64.cpp` is the arch sibling of the
6365-line arm64 lowerer.  It RUNS clean on x86 (the old "emits asmjit::a64
everywhere / SIGSEGV 0x358" note was stale — the arch-split predates it).
Still defaults OFF on x86 (DebugSettings kDefaultSistaOn only on __aarch64__);
enable with `PHARO_SISTA_DISPATCH=1`.

Baseline (fresh-image startup): OK=934 / bail=858 ≈ 52% compile rate.  The
bail histogram ([SISTA-BAILHISTO], emitted by the lowerer) showed the dominant
bail is `store_ivar` = 790 (92%) — a DELIBERATE safety bail on BOTH arches
(plain bytecode ivar stores fall back to tier-1; only setter-inline uses the
helper).  Not an x86 gap.  The real x86 gaps were small: guard_class (43),
prim_at (21).

Ported to x86 (all on origin/jit-x86): kGuardClass, kPrimAt, kPrimSize,
kPrimAtPut, kPrimAdd/Sub/MulFloat, kPrimEvenOddCheck, kLoadTempInVec,
kStoreTempInVec, kInterval (no-op marker), kBlockCreate.  Verified no-crash with
all ops on across ArrayTest/OrderedCollectionTest/FloatTest/IntervalTest under
PHARO_SISTA_DISPATCH=1 (guard_class+prim_at also startup-verified, OK 934→998).
Per-op disable gates for bisection: PHARO_SISTA_NO_LOWER_{AT,SIZE,ATPUT,FLOAT}.

ALL non-loop Sista ops are now ported.  Only the counted-loop fusions remain.

Crash fixed: the inline kPrimAt scale-3 indexed LOAD (`mov dst,[rcv+i*8]`)
crashed asmjit's register allocator (BaseRAPass::build_liveness null-deref) at
COMPILE time on collection tests — found via lldb + per-op env bisection.
(atput's scaled STORE is fine; only the indexed load tripped it.)  kPrimAt now
routes through the jit_rt_sista_basic_at helper (deopt-on-zero, like kPrimSize).

Verification harness: `scripts/aws/sunit-sista-verify.sh` runs an SUnit subset
with/without Sista and diffs; `validate_smalltalk_image` (separate repo) can
SHA-256-manifest snapshots for bit-level state diffing.

## Counted-loop fusions — verification harness + status (2026-06-04)

Box stability and a fast correctness harness unblocked this (see
`sista-x86-port` memory).  Box: the AWS instance has NO SWAP, so a runaway
fusion OOM-killed sshd ("box lost", ssh 255).  Fixed with a 16 G swapfile +
`scripts/aws/box-safe-run.sh` (runs the VM in a systemd transient service:
MemoryMax=8G MemorySwapMax=0 CPUQuota=400% RuntimeMaxSec — cgroup-kills a
runaway, never the box).  Harness:
`scripts/pharo-headless-test/sista_loop_bench.st` runs each idiom in a hot
400k-iter loop via dedicated helper methods, checks a known answer, writes
/tmp/sista_bench.txt, then quits (no scheduler hang, ~30 s).  Per-fusion EMIT
counters (`g_sistaEmit_countedLoop*`, printed under PHARO_SISTA_BAIL_LOG=1)
prove the fusion FIRED vs bailed to tier-1.

Run: prep a fresh image with stock pharo `eval --save` fileIn of the bench .st,
then `rm -f /tmp/sista_bench_done.txt` and
`box-safe-run.sh sfuse 120 PHARO_SISTA_DISPATCH=1 PHARO_SISTA_BAIL_LOG=1 -- ./build/test_load_image .../Pharo-bench.image`.

PORTED + VERIFIED-CORRECT on x86 (fusion fired + answer correct, EMIT counter):
    kCountedLoopInjectInto          inject_array=55       EMIT>0
    kCountedLoopArrayDoAccum        do_array=55           EMIT>0
    kCountedLoopIntervalDoAccum     do_interval=5050      EMIT>0
    kCountedLoopIntervalInjectInto  inject_interval=5050  EMIT>0
    kCountedLoopArrayCollect        collect_array         EMIT>0  (allocates result)
    kCountedLoopWhileTrueAccum      timesrepeat=1000      EMIT>0  (closed-form, c∈{0,1})
    kCountedLoopDo                  (no-crash only; non-accum do:, not on bench)
All 7 bench checks PASS together with all fusions on; box stable throughout.

REMAINING (2) — the two hardest, deferred:
    kCountedLoopArraySelect   arm64:4702  arr select:[...]  dynamic-size result.
       DEFERRED + arm64 BUG FOUND: arm64's select stores the k-th selected
       element at `result + writeIdx*8` (writeIdx 0-based) → first selected
       lands at byte offset 0 (the HEADER), off-by-one-slot vs Collect's
       `result + i*8`.  jit_rt_sista_array_shrink only rewrites the header
       slot-count (doesn't shift), so the trimmed array is wrong.  Likely a
       latent arm64 bug (select fusion rarely/never fires there).  An x86 port
       must use `result + 8 + writeIdx*8` AND handle the `e even`/compare
       predicate representation.  See docs/deferred.md.
    kCountedLoopIntervalDo    arm64:3783  (1 to:n) do:[...]  non-accumulating.
       Block body does sends / at:put: (no register accumulator); returns the
       interval — not cleanly verifiable by a known-output bench.  Lower value.
The WhileTrueAccum arithmetic-series shape (`s := s + i`) is also deferred: its
overflow check needs a 128-bit signed multiply (arm64 smulh) — x86 one-operand
imul into RDX:RAX is awkward under asmjit's Compiler RA.  Bails to tier-1.
Methods bail safely to tier-1 without any of these.
    store_ivar plain stores The 92%-dominant bail.  Routing plain bytecode ivar
                            stores through jit_rt_store_inst_var (as setter-inline
                            does) would lift the compile rate ~52%→~99% on BOTH
                            arches — but it's a deliberate shared safety bail;
                            needs a correctness decision, not just an x86 port.
    %lld format warnings    ~33 -Wformat warnings: `%lld` vs int64_t (long on
                            x86_64).  Non-blocking (no -Werror).  Fix with PRId64.

## Next steps

1. Port the remaining Sista ops above; verify each via sunit-sista-verify.sh.
2. Decide on store_ivar plain-store lowering (big shared compile-rate win).
3. Clean up the `%lld`/int64_t format warnings (PRId64).
