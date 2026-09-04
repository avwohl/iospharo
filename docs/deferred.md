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

## Tooling limitations worth knowing

- **`PHARO_DET_SCHED` starves cross-thread wakes**, so it is unusable on FFI /
  callback bugs. Worth flagging because CLAUDE.md tells sessions to reach for
  it first on any timing-sensitive bug — it is the right first move for
  interpreter-level Heisenbugs and the wrong one for anything involving a
  worker thread.
- **MSYS2 login-shell environment stripping breaks Pharo's WindowsResolver.**
  Operator documentation, not a code gap.
- **`scripts/aws/preserve.sh` can fast-forward a *revert* onto shared `jit`.**
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
  tenures everything it touches.  Not a leak and not a correctness bug;
  unquantified, like the `step()` cost above.  The low-space breaker's latch
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

## Platform validation status

- **Windows has no CI job and has not been validated in ~250 commits** (last
  Windows source commit 2026-07-04). Every Windows "FIXED" verdict in
  `docs/history/windows-port-2026-06-27.md` means *fixed as of July*, against a
  binary that default-on `becomeForward` and the GC context-reuse changes have
  landed on top of untested. Either add a Windows build job or read that file
  as dated.
