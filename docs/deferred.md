# Deferred / not-100% work

Intentional gaps: platform stubs, declined primitives, known limitations, and
policy decisions. Everything here is a **choice**, not a defect.

If you are looking for something that is broken, it is not in this file:

    what is happening right now                 docs/WIP.md
    open VM defects (our VM fails, Cog passes)  docs/vm-compat-bugs.md
    bugs in the Pharo image we patch            docs/image_issues.md
    what was fixed and when                     docs/changes.md, docs/history/
    suite + package measurements                docs/test-results.md, docs/results/

(The `WIP.md` at the repo root is a pointer only — the file that carried state
there was three weeks stale on 2026-09-02 and is now in `docs/history/`.)

This file was 2739 lines on 2026-08-12, of which 86% was changelog and
debugging narrative, and 14 real defects were buried in it — five of them
inside entries whose parent was checked `- [x]`. It was split on that date.
The history lives in `docs/history/`; nothing was deleted.

## Graphics / display

- **`primitiveWarpBits` declines `n < 1 || n > 4` and non-32-bit surfaces**
  (`Primitives.cpp:21892`). Cog's BitBltSimulation has no such cap, so for
  those cases the image drops into a Smalltalk fallback that Cog never
  executes. Deliberate.
  Raising the cap is NOT a free win: it would hide open defect #1 in
  `docs/vm-compat-bugs.md` (the WarpBlt expression-stack displacement is only
  visible *because* the image takes that fallback), and it would do so while
  looking like a 99-error improvement.
- **Text input / IME is a no-op** — on Windows and on Linux.
- **WorldRenderer native draw uses the Linux stub on Windows.**

## Audio / device

- **MIDIPlugin is stubbed on all non-Apple platforms.**
- **SoundPlugin's waveOut backend is implemented but never runtime-verified** —
  stock Pharo 13 ships no sound package to drive it. (Also lead 22 in
  `docs/vm-compat-bugs.md`: "never executed" is a gap in evidence as well as a
  deliberate stop.)

## Memory / scheduling policy

- **The low-space watcher is inert unless the image arms primitive 125.** This
  is Cog-identical behaviour, but the consequence is worth stating: every
  headless `eval` run has no OOM defense. The 2026-08-11 package sweep lost
  three packages to an OOM kill for exactly this reason.
- **Windows old-space commit policy** — see open defect #10. Reserving the
  whole space up front is deliberate; committing it unconditionally is the bug.

- **Three remaining "pc between an extension prefix and its bytecode" paths**
  (from the 2026-10-12 audit workflow; the two live ones are fixed, see
  `docs/perf-activation-measurements.md`, "The internal-J2J crash").  None of
  these runs in the default configuration with standard Pharo methods:
  - x86 `PHARO_T1_EXTBAIL_SEND=1` (bisect knob, default off) turns the
    prefix bail into an ExitSend.  The send handlers then dispatch a stale
    selector and advance ip by 1 into the prefix.
  - ~~`materializeFrameStack`'s fd==0 PushThisContext rewind tests the raw
    byte before ip.~~  FIXED 2026-10-13: the rewind also requires
    `!inExtension_` and decodes the method from its first bytecode to confirm
    the byte before ip starts an instruction (only in the rare state that
    reaches the check: the Context itself on top of its own stack).
  - `SistaBuilder` marks "inside a prefix" by a non-zero pending value and
    OVERWRITES chained prefixes (`pendingExtA_ = byte`; the interpreter
    shifts).  Only methods with 8193+ literals or very long jumps emit
    chained prefixes.

## Tooling limitations worth knowing

- **`PHARO_DET_SCHED` starves cross-thread wakes**, so it is unusable on FFI /
  callback bugs. Worth flagging because CLAUDE.md tells sessions to reach for
  it first on any timing-sensitive bug — it is the right first move for
  interpreter-level Heisenbugs and the wrong one for anything involving a
  worker thread.
- **MSYS2 login-shell environment stripping breaks Pharo's WindowsResolver.**
  Operator documentation, not a code gap.
- **~~`scripts/aws/preserve.sh` can fast-forward a *revert* onto shared `jit`~~
  — FIXED 2026-09-04.**  The autosave is no longer in git, and the script is
  no longer in this repo: the spot box lifecycle moved to `avwohl/aws_watch`
  under `spot/`, where it is project-independent and sits beside the reaper and
  lease table these boxes already register with.  `preserve.sh` builds the
  snapshot with plumbing in a scratch index (so the box's HEAD, index and
  working tree never move) and writes it to
  `s3://$BUCKET/autosave/<instance-id>/<ts>/` as a git bundle, beside a plain
  `wip.diff`, `status.txt` and a manifest.  Committed work still goes to
  `WORK_BRANCH` as an ordinary push, where a stale box is correctly refused as
  a non-fast-forward.  Recovery is `aws_watch/spot/restore-autosave.sh`, which
  lands a dump at `refs/autosave/<instance>/<ts>` and moves nothing else.

  An intermediate version of this fix pushed the dump to a per-box
  `autosave/<instance-id>` branch instead.  That closed the clobber but not a
  second problem: a git object is permanent, so every dump's blobs would stay
  in the pack forever, `add -A` sweeps in whatever untracked build output
  `.gitignore` misses, and a stale autosave branch is the ref nobody deletes.
  An S3 object is deletable and can be expired by a lifecycle rule.  For scale:
  the two autosaves that reached `jit` cost 540 KiB of permanent pack, against
  20 MiB of `build*/` binaries committed and later untracked — untracking
  reclaims nothing.

  `aws_watch/spot/test-preserve.sh` now covers all of this against a throwaway
  repo and a stub `aws`, including the `ede0fd65` clobber itself.  The move
  also fixed a second live defect it exposed: the idle timer runs as root, but
  the git deploy key is in the box user's `~/.ssh`, so an idle-triggered
  preserve's push could never authenticate — only the S3 half of the dump
  survived, and only the spot watcher (which runs as `ubuntu`) ever pushed.
  `idle-shutdown.sh` now drops to `$SPOT_USER` for the preserve.

  The two related exposures are unchanged and still true: `origin/jit` has no
  branch protection, and nothing watches for an autosave landing on it.
  Original text follows.

  `scripts/aws/preserve.sh` can fast-forward a *revert* onto shared `jit`.
  It snapshots the working tree with `git add -A` but commits it with the
  box's HEAD as parent, and those two can disagree arbitrarily.  On
  2026-09-03 the disagreement was benign — stale parent `52f6f40c`, current
  tree copied in by hand — and produced `ede0fd65`, a single squashed
  snapshot on the shared branch.  The dangerous direction is a box that
  clones the current tip and then receives a *stale* tree: `add -A` records
  the deletions, the parent is the true tip, and the push is a clean
  fast-forward GitHub accepts with no force flag.  `git diff --name-status
  e4c47863 ede0fd65` already shows `D scripts/sweep-3way.sh` as a live
  example of that shape.  The submodule guard added after `27d378d2`
  protects only the two `.gitmodules` paths and does nothing for ordinary
  source files.  Fix, not applied: point the autosave at a per-box
  `autosave/<instance-id>` namespace so a crash dump can never reach a ref
  anyone builds from.  Related: `origin/jit` has no branch protection and the
  box holds a write-capable deploy key, and nothing detects an autosave
  landing on `jit` — the previous one (`27d378d2`, 2026-06-03) reverted the
  asmjit Catalyst pin and went unnoticed for 68 days.
- **Eleven submodule pins in already-public history are dangling**, so
  `git submodule update` fails anywhere before `309ab535` (2026-09-02).
  `749ce144` is the famous one — it is what made `origin/jit` un-cloneable
  until 2026-09-03, and CI's "Fresh clone builds" job had been failing at
  `actions/checkout` with `upload-pack: not our ref 749ce144` since
  2026-08-23 — but `e1c44a2b` alone is pinned by 526 commits, plus
  `7ae82806`, `74582025`, `b097978c`, `c212d540`, `4e8a4b9c`, `cde7ac7e`,
  `808c2118`, `725e3e28`, `18308c35`.  They were lost when
  `scripts/pharo-headless-test` was re-cloned on 2026-09-02; the work in
  `749ce144` was re-created as `ed885c81`.  A tip clone is unaffected.  A
  `git bisect` that ranges back through pre-2026-09-02 history is not.
  Recovering them needs the objects (an old clone, or the S3 preserve
  bundles); repairing history needs a rewrite of a public branch.  Both are
  separate decisions.
- **`sync-repos.sh` cannot see submodules.**  `discover_repos` globs one
  level under `$SRC_ROOT`, so `scripts/pharo-headless-test` — whose nine
  unpushed commits are what made `origin/jit` un-cloneable — is invisible to
  both its branch report and its unpushed-commits report.  A green
  `sync-repos.sh --status` does not mean the tree is fully pushed.

## Known structural costs, not yet measured

- **T1 send sites are 2.3-2.9 KB each (measured 2026-10-15).**
  `PHARO_T1_EMIT_SIZE_CENSUS=1` on lexicon: a 0-argument send site emits
  ~2270 bytes, a 1-argument site ~2880 (570-720 instructions), and sends
  are ~90% of all machine code (34.7 MB for 5120 methods).  Cog emits tens
  of bytes per site.  With every inline specialization off (getter,
  setter, at:/at:put:/size, bit operations, returns-literal/self,
  basicNew:) a site is still 1.7-1.85 KB: the bulk is the base machinery
  repeated at each site (the native call path, the call-cache special
  kinds, the primitive stub and C-call paths, the failure body call, the
  exits).  The fix is a compact emit: the IC check and the native-call
  setup inline, everything else in shared per-arity stubs taking the
  site's IC entry, continuation and fallback in registers.  Payoff for
  speed looks small (lexicon IPC is 4.3, compiles are already fast); the
  code zone default went to 192 MB instead (c32bb05e).  Do this before
  shrinking the zone again or targeting small-memory devices.
- **Two-way become of OLD objects of different sizes is O(heap)
  (2026-10-15).**  9f0d91f9 made young pairs cheap (copies, forwarders,
  one scavenge) but the scavenge tenures the pair, and ReStore swaps the
  same proxy/object pairs again: each later swap is a parallel full-heap
  scan (~2.3 ms).  ReStore writes 1000 objects in 2.4 s (Cog 63 ms) and
  its SSWReStoreSpeedTest assertions fail on time.  Cog forwards lazily:
  the old objects become forwarders to copies, and sends, ==, primitive
  failure and the GC follow them.  This VM follows forwarders only in a
  few places (classOf, prim 148), so lazy forwarding needs a send-path
  receiver fix-up, a forwarder-aware ==, primitive-failure retry and a
  GC that removes forwarders.  The same cost fails StSpotterModelTest (12
  one-way becomes per `StSpotter new`, ByteString -> WideString).
  Design notes for the change (old objects only; young pairs already go
  through a scavenge):
  - Lazy path only for plain objects: not methods, classes, symbols,
    MethodDictionaries, contexts, processes, semaphores or literal
    variable bindings (pushLitVar reads the binding's slot directly).
  - A forwarder gets a reserved format as well as the forwarded class
    index, so every format-checking fast path (interpreter special sends,
    inline at:, primitive stubs, mcfFastReplace) rejects it.
  - A global "forwarders exist" flag, cleared by the full GC that removes
    them, keeps the new checks off the hot paths otherwise.
  - Follow (and write back) at: interpreter sends before the method
    cache probe; pharo_mcf_perform and mcfExtSendTarget (they look up
    through classOf, which follows, then call natively with the forwarder
    as receiver -- refuse instead); block activation (a closure's receiver
    field); executePrimitive's receiver and arguments; `==`/`~~` on a
    mismatch (interpreter, inline T1, prim 110); inline `class`.
  - After installing forwarders, scan the frames and machine-code
    registers (scanStackReplace/Swap) so no frame holds one.
  - FIRST the full GC: ObjectMemory::markAndTrace returns early for a
    forwarder (ForwardedClassIndex has no class-table entry), so it does
    not trace the target through it or rewrite the reference.  Safe
    today only because every become scans away all references before it
    leaves a forwarder.  Lazy forwarding needs marking to follow a
    forwarder to its target and the slot to be rewritten (Spur does this
    in its mark loop), then the forwarders freed.

- **~~Every scavenge scans ALL of old space for old->young pointers~~ --
  DONE 2026-10-13 (`064e1de6`): the remembered set is trusted by default
  (every T1 store barriered; opt-out `PHARO_SCAV_FULL_SCAN`, check
  `PHARO_SCAV_TRUST_VERIFY`, zero misses over full sweeps).  History:**
  (`ObjectMemory::scavenge`, the `scanRegionForYoung(oldSpaceStart_, ...)`
  call), because the remembered set is not trusted: measured 2026-10-12 with
  `PHARO_SCAV_BIT_AUDIT=1` on StringTest + OrderedCollectionTest + 100
  compiler evaluations, 276 old objects in 16 scavenges held a young pointer
  with the remembered bit CLEAR (Dictionary, Array, OrderedDictionary,
  Context, FFI parser objects).  The scan was ~13% of a native profile of
  `NoUnusedVariablesLeftTest` (a TIMEOUT).  Trusting the remembered set
  needs every store path audited -- interpreter store bytecodes, JIT
  stencils, primitives that `slotAtPut` directly -- until the audit reports
  zero gaps over a sweep.  Same root blocks a cheap young-only become (#2
  batch 5 newtools paragraph in `docs/vm-compat-bugs.md`).
  Breakdown (same workload, PHARO_NO_JIT, so none of it is JIT stores):
  230 of 290 gaps (79%) are holders allocated in OLD space since the last
  scavenge (eden-full fallback, overflow objects, old clones) and filled
  with young pointers -- the audit now reports them as `gapRecent`, and a
  scan of [oldFreeAtLastScavenge_, oldSpaceFree_) would cover them.  Of the
  60 older holders, 43 are a Context's sender or a FullBlockClosure's
  outerContext pointing at a YOUNG Context (VM frame linking), 5 are
  CompiledMethod literal 2 = a young TFExternalFunction (FFI), and the rest
  single instance-variable stores.  Every interpreter store path checked
  (setReceiverInstVar, storePointer, storePointerUnchecked) does apply the
  barrier and isYoung/isOld use the scavenger's bounds, so the older gaps
  are either writes outside those helpers or a header rewrite that drops
  the remembered bit -- not yet found.

  **2026-10-13: the older-holder gaps were mostly the audit's own mark, and
  the real blocker is the JIT.**  Three fixes, then a re-audit:
  (1) the remembered BIT was never cleared, so after an object's first
  old->young store `rememberObject` never recorded it again; the full scan
  now clears it per object, so a set bit means "barriered since the last
  scavenge".  (2) `oldFreeAtLastScavenge_` was set only by scavenge, so after
  a full GC compacted below it every new old-space object counted as an
  older holder; `fullGC` now resets it.  (3) the last interpreter-side gaps
  were raw `slotAtPut`s: `setSpecialObject`, both become loops, the
  forwarder slot, and the class-table pages -- now `slotAtPutBarriered` /
  `storePointerUnchecked`.  Re-audit (`PHARO_SCAV_BIT_AUDIT`, knobs-on
  build) on 3M printString, the cormas analysis, the myprecious round trips
  and `bench_ast_phases.st`: zero holders outside the remembered set and the
  recent region (`gapNoBarrier=0` after fix 3, `gapBitOnly=0`).  A new knob,
  `PHARO_BARRIER_WHO`, backtraces the raw `slotAtPut` behind any gap.
  What still blocks trusting the set is emitted code: asmjit T1 stores with
  NO barrier at ExtPopStoreLitVar/ExtStoreLitVar (globals and class
  variables), ExtPopStoreRecv/ExtStoreRecv, the inline setter
  (`PHARO_T1_SETTER_BARRIER` is off), and remote-temp stores (0xFC/0xFD);
  popStoreRecv sets the bit but never records the object.  The audit stays
  clean only because T1 now compiles little (loops only).  The plan that
  fits: one emitter for an inline barrier that sets the bit and appends the
  holder to a fixed global buffer (overflow sets a flag that makes the next
  scavenge a full scan), called from every store emit on arm64 AND x86;
  scavenge drains the buffer into `rememberedSet_`.  Gain: the old-space scan
  is 11.8% of the cormas analysis and 5% of the myprecious round trips.

  **Implemented the same day; default ON (opt-out `PHARO_SCAV_FULL_SCAN`,
  runtime).**
  `emitRememberBarrierA64` / `emitRememberBarrierX86` (AsmjitT1.cpp) at every
  T1 store into a heap object (arm64: popStoreRecv, Ext(Pop)StoreRecv,
  Ext(Pop)StoreLitVar, remote-temp stores, inline setter, prim-61 prologue,
  inline send-site at:put:; x86: popStoreRecv, inline setter, LitVar,
  remote-temp) append the holder to `g_jitRememberBuf`; overflow forces a full
  scan.  The trusted scan runs only when every emitter that can run is
  barriered (`emittedStoresBarriered`: asmjit T1, Sista dispatch off -- now
  the default, see DebugSettings.cpp -- and Tier2 off).
  `PHARO_SCAV_TRUST_VERIFY` (runtime) re-walks old space after each trusted
  scan and reports + repairs any missed holder.  Zero misses so far on
  printString, cormas, myprecious, bench_ast_phases (arm64 and x86) and the
  VM fix battery; the full-sweep verify run decides the default.
  cormas analysis: 23.84 -> 22.30 billion instructions.

- **A preempted method finishes in "context mode" (found 2026-10-13).**  A
  process switch materializes the outgoing process's whole frame stack
  (`transferTo` -> `materializeFrameStack`), and the process resumes with
  `executeFromContext` at frame depth 0.  The method that was running keeps
  executing at depth 0 until it returns: every temp store is mirrored into
  its Context (`setTemporary` -> `storePointerUnchecked` +
  `raiseContextStackpTo`) and every send stores the pc into it (`pushFrame`,
  depth 0 -> 1).  For a long loop that is ever preempted -- any loop longer
  than one Delay tick in a process below timing priority -- that is the rest
  of its run.  The lldb step trace of `zzOuter:` (a 2e9-iteration loop
  method called from an eval) showed it at depth 0 within seconds.  Block
  creation does the same (`materializeFrameStack` on every closure push at
  depth > 0).  Cog avoids both with married contexts: the frame stays the
  truth and the Context is a proxy, divorced only when image code needs it.
  The port needs every Context field access (instVarAt:, the receiver-
  variable bytecodes in Context methods, the debugger's primitives) to go
  through a "sync from frame" check.  Multi-day.  **Measured the same day
  (`[DEPTH0]` exit line, bytecode checkpoints at frame depth 0): myprecious
  round trips 2.5%, lexicon CodeAnalysisRunnerTest 1.3%, deeptraverser 0.1%.**
  So it matters for one long loop that gets preempted (the micro-benchmarks),
  not for the slow packages.  **Correction, same day:** an earlier version
  of this note said a preempted long loop runs "5x slower and is never
  JIT-entered" in context mode.  That compared a 21M-iteration method loop
  with a DoIt loop that ran only 1.2M iterations (it scanned printString's 4
  bytecodes, not 70).  Measured properly: `Interpreter::resumeAsFrame` (the
  resumed method pushed as a frame above its sender, its Context kept as
  currentFrameMaterializedCtx_) took the loop's depth-0 share from 47% to
  0.06% and its time from 601 to 595 ms -- no gain -- so it was reverted.
  The loop is 28 ns per iteration on ours, 4.5 on Cog, with the JIT on or
  off: plain interpretation, and T1 does not speed it up.  For the slow
  packages context mode does nothing either; their cost is ordinary interpretation, chiefly
  block calls (~700 instructions each; Fuel's `isBehaviorAnonymous:` scans
  all of `Smalltalk globals` with a block per element on every myprecious
  message).
- **Smalltalk that runs inside an FFI callback executes through
  `Interpreter::step()`, one bytecode per call**, not through the
  computed-goto dispatch loop: `enterInterpreterFromCallback` is
  `while (running_) { for (batch < 1000) step(); ... }`.  Every callback the
  image services — Iceberg/libgit2 progress callbacks, SDL, TFFI callbacks —
  pays whatever `step()` costs over the fused loop, and nobody has measured
  what that is.  Noticed 2026-08-22 while sampling a slow `Fuel` package
  load; in THAT case the process turned out to be asleep in
  `primitiveRelinquishProcessor` waiting on `http_stream_read`, i.e. network
  I/O and not this, so the cost remains unquantified.
- **Eden is never scavenged while an FFI callback is running.**  Same loop,
  second cost, found 2026-09-02 while auditing what the nested loop omits.
  `needsScavenge_` is set by `allocateRawYoung` when eden fills
  (`ObjectMemory.hpp:856`) and is consumed at exactly one place — the
  `interpret()` checkpoint (`Interpreter.cpp:3877`) — which does not run while
  `enterInterpreterFromCallback` hosts execution.  `step()` does handle
  `needsCompactGC()`, so the heap does not grow without bound; what happens
  instead is that once eden fills, every allocation for the rest of the
  callback falls back to old space (`allocateRawYoung` returns nullptr and the
  caller tenures on the spot) and is reclaimed only by a full compacting GC.
  So a long callback runs the expensive collector in place of the cheap one and
  tenures everything it touches.  Not a leak and not a correctness bug.
  **Quantified 2026-10-13** (both costs): `FFICallback primQsort:` over 5000
  doubles with a Smalltalk comparator, 49391 callbacks: 175 us per callback
  on ours, 10.7 on Cog.  Profile: ~28% `step()`/`dispatchBytecode`, ~30% full
  GC (`markPhase`, compaction).  Each callback also runs at least one
  1000-step batch plus a 500-step cooldown, whatever its block needs.
  **Scavenge part FIXED the same day:** the batch loop now scavenges when
  eden is full (clearing `needsScavenge_` first, as the interpret()
  checkpoint does -- a first try that did not clear it rescanned after every
  batch and looked like a wedge).  95 us per callback.  Still open: ~49% of
  what is left is `step()` + `dispatchBytecode` (the switch path, not
  interpret()'s fused loop) and ~4% is the C++ exception that ends each
  callback (libunwind + dyld unwind-info lookups).  The 1000-step batch is
  NOT a cost: batches of 200 or 50 measured 99 and 104 us.  Running the
  handler through interpret() needs interpret() to be re-entrant with a
  "callback returned" exit at its checkpoint.  The low-space breaker's latch
  IS delivered from this loop as of `3c75aca5`, which matters more here than
  on the main path precisely because everything allocated here is tenured.

  **And the window is not one callback long.**  `callbackDepth_` is decremented
  only by `primitiveCallbackReturn` and the two worker-timeout
  `[XTCB-DEAD-POP]` paths, so an invocation the image abandons is never popped
  — `TFCallbacksTest`'s old-session test does exactly that, by design.  Sweep
  batch 1801-1850 therefore runs its remaining ~25 test classes INSIDE the
  nested loop: no scavenge (everything tenured on the spot), no `interpret()`
  checkpoint, and until `3c75aca5` no low-space latch either.  So this is not a
  cost paid by the odd long callback; it is a mode a whole batch can fall into
  and stay in.  Defect #26 in `docs/vm-compat-bugs.md` covers the quit half of
  the same stuck counter.

- **A block called from T1 code costs more than from the interpreter
  (measured 2026-10-13).**  `blockbench.sh` (`b value: i` in a counted loop,
  per call, instructions retired): JIT on 1002, `PHARO_NO_JIT=1` 698.  The
  loop method has a loop, so T1 compiles it; each `value:` then leaves
  emitted code, activates the block in the interpreter and resumes the
  emitted caller.  Every `do:` / `collect:` / `inject:into:` is this shape.
  On whole workloads the JIT is net neutral (printString 91.3 vs 93.2
  billion; deeptraverser 1147 vs 1138; myprecious 49.1 vs 49.4), so this is
  a lost win, not a regression.  Fix candidates: compile the block too and
  call it J2J, or keep loops whose body is a block `value:` out of T1.  Tried
  the second (refuse at `compile_check`, which OSR also reaches): WORSE, 1069
  vs 1011, while `PHARO_NO_JIT=1` gives 698 on the same benchmark.  Cause not
  found: the depth-0 share is similar in both modes (`[DEPTH0]` 40% vs 34% of
  checkpoints).  Find what the JIT-on interpreter path does per block call
  that the no-JIT one does not before trying again.
- **DONE 2026-10-14: T1 lost on send-bearing code because of the
  interpreter<->JIT transitions.**  Replaced by machine-code frames
  (`docs/mc-frames-design.md`), the arm64 default: one frame model, exits
  at bytecode boundaries, memo returns, native calls of methods, primitives
  and blocks.  printString 83.7 -> 62.1 billion instructions, deeptraverser
  1004 -> 764, lexicon 1699 -> ~1080.
- **Block creation materializes the whole frame stack (lexicon: ~10% of
  samples, 2026-10-14).**  `createFullBlockWithLiteral` needs an outer
  context for a non-clean block and calls `materializeFrameStack()`, which
  makes contexts for every frame not yet synced, because a context's sender
  must be a context.  Cog marries only the current frame and materializes
  senders on demand.  Doing the same here (a context whose sender is "the
  frame below", resolved when read) is the lazy-context redesign; it would
  also cut `widowReturningContext` and process-switch cost.  Census
  (`PHARO_MATFS_CENSUS`, lexicon, 2026-10-14): 51 million calls; each walks
  16.6 frames, of which 14.5 are an already-synced prefix and about 2.1
  are re-synced (the current frame's temps and stack copied into its
  context, and new contexts allocated).  Skipping the synced prefix would
  save about 1%; the snapshot copy and the allocations are the cost, which
  only married (read-through) contexts remove.
- **`PHARO_NO_JIT=1` still pays `noteMethodEntrySlow` on every activation.**
  The JIT runtime initializes with the compiler off, `countThresholdCache_`
  stays 0, so the inline gate never takes its fast path.  Diagnostic mode
  only; makes no-JIT instruction counts read ~5% high.

## Native returns are not predicted (measured 2026-10-16)

A native call is a `b` (or `br x16` to a stub) with the continuation in
x14, and the return is `br x9` with the record's memo.  Neither touches the
return-address stack, so the CPU predicts each return with its indirect
branch predictor.  A method whose callers sit at different sites (benchFib
calls itself from two sites) returns to a target that changes from call to
call.

Evidence.  Fibonacci with two recursive call sites (`fibA:`) runs about 105
instructions per call in 4.0 ns.  The same recursion with one call site in a
loop (`fibB:`) runs about 144 instructions per call in 4.3 ns: about 25% more
work per nanosecond.  Cog (bl/ret) does `fibA: 30` three times in 20 ms;
we take 32 ms.  A single-site recursion is faster here than on Cog.

The fix is call with `bl`/`blr` and return with `ret x9`, which needs x30
free inside machine code.  Today every exit returns to the trampoline with
`ret x30`, so the exits must take the trampoline's return address from
another place.  Touch points:
- The trampolines `pharo_jit_osr_resume` and `pharo_jit_j2j_trampoline`
  (TrampolineAsm.S) and the `JIT_CALL` macro (JITState.hpp, 12 call sites;
  its operands may sit in x28, so x28 cannot simply hold the address).
- About 40 exit sites (`a.ret(x30)` in emitOne_arm64, emitMethodBytes,
  simGenericExit, emitMcfHelperSend, the shared return prelude and the NLR
  stub).  The internal stubs that return to their call site (primitive,
  tiny-shape and block-create stubs) keep `ret x30`.
- The linked send site's W2 word becomes `bl` (JITRuntime.cpp
  linkSendSiteMcf writes `encodeB`; the decoders test `isB`), and the
  memo must be the address after the call instruction.
- Nested C -> JIT -> C -> JIT entries must save and restore whatever holds
  the trampoline return.

Expected gain: up to about 1 ns per call where returns mispredict; lexicon
makes about 1e9 calls in about 13 s.

**Tried 2026-10-16: no gain.**  A full prototype (exits through a
trampoline-return field in JITState set by every entry, calls with bl/blr,
returns with `ret x9`; single-step traces confirmed bl/ret pairs) left
benchFib, fibA and fibB at the same wall time.  The return predictor was not
the bottleneck; reverted.  The next suspect is the per-call dependency chains
through memory: the checkpoint countdown (load, subtract, store on every
call), frameDepth_ and currentFrameMaterializedCtx_ (load and store on push
and on return).  Each call waits for the previous call's store.

## The primitive surface is much larger than the primitive table

Audited 2026-09-02, recorded so nobody re-derives it and reads it as breakage:
`Interpreter.hpp` declares **828** `primitive*` members and only **460** are
reachable through `primitiveTable_` (`src/ios/generated_primitives.inc` plus
the explicit assignments in `Interpreter.cpp`).  The 368 that are not fall into
three groups, all deliberate:

  * **The iOS device surface** — Camera, MIDI, IAP, Notification, Keychain,
    Location, Joystick, Serial, Sound mixer, SSL, Clipboard, Share, Haptics.
    Nothing in a desktop Pharo image calls these, and the named-primitive
    registry (`registerNamedPrimitive`, `InterpreterProxy.cpp`) is populated
    from the plugin tables with free functions, not these members.
  * **Squeak-era slots Pharo 13 does not use.**  `primitiveScanCharacters` is
    the clearest: it is declared `// 103`, fully implemented, and
    `primitiveTable_[103] = nullptr` — and the image contains **zero**
    `<primitive: 103>` call sites, so wiring it would change nothing.  (Worth
    stating because the character scanner is the glyph path and defect #19 is
    about glyph drawing being 12-25x slower; this is not that.)
  * **Helpers reached by direct call rather than by index** — e.g.
    `primitiveShallowCopy`, called from `primitiveClone`.

One member was genuinely dead and is gone (`primitiveBecome`, `c4f4106c`): no
slot pointed at it, nothing called it, and Spur has no scalar two-way become
for it to be.  The audit found no other case of an implemented primitive that
the image actually calls and the table leaves null.

### Named (plugin) primitives: the image calls some that the VM lacks

Audited 2026-10-15 on the polymath image (312 named primitive call sites,
288 distinct module:name pairs).  A named primitive the VM does not register
runs the image's Smalltalk fallback; since 475c32c2 the failed lookup is
cached, so it costs only the fallback.  Missing pairs that Cog HAS (its
`Plugins/` folder is the reference):

  * **LocalePlugin** (8 functions: primDST, primLongDateFormat,
    primShortDateFormat, primMeasurement, primCurrencyNotation, ...).  Ours
    answers the fallbacks: DST false, `dddd, mmmm d, yyyy` and `m/d/yy`
    formats, metric true.  Cog asks macOS: DST true, `y MMMM d HH:mm:ss z`,
    `y-MM-dd HH:mm`, metric false.  No sweep test fails on it.  Needs
    CFLocale on Apple platforms and a POSIX answer elsewhere.

Missing pairs Cog does NOT have either (not a gap): ZipPlugin, MD5Plugin,
Matrix2x3Plugin, FontPlugin, BMPReadWriterPlugin, AsynchFilePlugin,
InternetConfigPlugin, KedamaPlugin.  SocketPlugin and SqueakSSL register
through their own tables, so a grep for registerNamedPrimitive misses them.

## Platform validation status

- **Windows has no CI job and has not been validated in ~250 commits** (last
  Windows source commit 2026-07-04). Every Windows "FIXED" verdict in
  `docs/history/windows-port-2026-06-27.md` means *fixed as of July*, against a
  binary that default-on `becomeForward` and the GC context-reuse changes have
  landed on top of untested. Either add a Windows build job or read that file
  as dated.
