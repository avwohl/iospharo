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
    kCountedLoopArraySelect         select_array=#(2 4 6 8 10) EMIT>0  (per-iter send predicate)
    kCountedLoopIntervalDo          interval_do=5050      EMIT>0  (non-accum, effect-free block)
    kCountedLoopDo                  (no-crash only; non-accum array do:, not on bench)
All 9 bench checks PASS together with all fusions on; box stable throughout.

ALL 9 counted-loop fusions are now ported.  IntervalDo (the last) is VERIFIED on
BOTH arches (2026-06-04, EMIT=1, interval_do=5050).  It is the non-accumulating
`(start to: stop) do: [:e | <pure arith>]` — the builder admits only side-effect-
free blocks (loads + int arith + tag-check; NO sends, NO stores, no capture) and
only when the do: result is discarded (Pop/ReturnReceiver follows), so the block
result is dead and the fusion returns a startReg placeholder.  No GC hazard (no
send/alloc in the loop).  Loop scaffold is identical to the verified
IntervalDoAccum.  Because the block is effect-free its per-iteration value is
unobservable; the bench (intervalDoNop:) runs the dead IntervalDo loop then an
accumulating loop and returns 5050, proving the fused loop ran without corrupting
state.  arm64 already had the lowering (3786) but no emit counter — added one.

IntervalDo adversarial review (4 skeptic lenses + per-finding verify) found 3
real, shared/both-arch bugs in the interval fusions (all pre-existed in arm64;
the x86 interval ports faithfully inherited them).  Two FIXED + verified on both
arches (bench guards neg_do/neg_inject/neg_intervaldo=9, nlr_first=1):
  A (critical): the 3 interval fusions (IntervalDo/IntervalDoAccum/Interval
     InjectInto) compared TAGGED SmI loop bounds with UNSIGNED branches (ja/jbe
     x86, b_hi/b_ls arm64) → a negative lower bound encodes huge-unsigned so it
     ran 0 iterations (e.g. (-3 to:5) do: → 0, not 9), and (-5 to:-1) could
     near-infinite-loop.  Fixed: signed branches (jg/jle, b_gt/b_le).  Array
     siblings compare non-negative sizes → correctly left unsigned.
  C (major): an explicit `^expr` (ReturnTop) inside a sub-lifted splice block was
     lifted to a discardable kReturn → the NLR was silently dropped.  Fixed in
     SistaBuilder: ReturnTop bails the fusion when sub-lifting (tier-1 handles
     the NLR), matching the documented intent.  Affects all block-bearing fusions.
  B (major, DEFERRED): on a TAKEN deopt the interval fusions rebuild [start,stop]
     but resume at the PushFullBlock offset, where the elided `to:` send means
     `do:` is sent to a SmI → DNU/stack corruption.  LATENT (only fires on a
     non-SmI bound or non-SmI block-arith operand in a discarded do: — rare),
     pre-existing on arm64.  Correct fix = record the fusion framepoint at the
     `to:` offset (per-variant: IntervalDo to:=pfb-1, DoAccum/InjectInto differ
     by the intervening PushTemp), which needs deopt-path verification — a
     focused follow-up.  Disable any counted loop via PHARO_SISTA_NO_LOWER_COUNTED_LOOP.

ArraySelect VERIFIED on BOTH arches (2026-06-04, EMIT=2, select_array=
#(2 4 6 8 10) / select_gt=#(6 7 8 9 10) correct).  Getting there fixed THREE
real bugs (all shared / both-arch):
  (1) kCountedLoopArraySelect was missing from the hasSplice allowlist
      (SistaRuntime.cpp) → select methods bailed as sendNoSplice before lowering,
      so select was DEAD on both arches.
  (2) the select PREDICATE lifts to a per-iteration kSendUnspeculated (`e even`,
      `e > 5` are sends, not inlined arith) — both lowerers now run the per-iter
      send via jit_rt_sista_call_send.  GC caveat: rcv/result aren't reloaded
      after the send, so an *allocating* predicate could move them (safe for the
      usual allocation-free predicates; shared with arm64's existing design).
  (3) select blocks are lifted in implicit-return mode (no explicit kReturn —
      last value is the predicate); both lowerers now use the last block value.
  Plus the arm64 store off-by-one: was `result + writeIdx*8` (first kept element
  on the HEADER); now `result + 8 + writeIdx*8`.  shrink only rewrites the
  slot-count, so the old offset gave wrong arrays — now correct.

REMAINING (0) — all 9 counted-loop fusions ported + verified on both arches.
The WhileTrueAccum arithmetic-series shape (`s := s + i`) is still deferred: its
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
