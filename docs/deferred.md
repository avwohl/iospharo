# Deferred / not-100% work

Intentional gaps: platform stubs, declined primitives, known limitations, and
policy decisions. Everything here is a **choice**, not a defect.

If you are looking for something that is broken, it is not in this file:

    open VM defects (our VM fails, Cog passes)  docs/vm-compat-bugs.md
    bugs in the Pharo image we patch            docs/image_issues.md
    what was fixed and when                     docs/changes.md, docs/history/
    suite + package measurements                docs/test-results.md, docs/results/

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

## Platform validation status

- **Windows has no CI job and has not been validated in ~250 commits** (last
  Windows source commit 2026-07-04). Every Windows "FIXED" verdict in
  `docs/history/windows-port-2026-06-27.md` means *fixed as of July*, against a
  binary that default-on `becomeForward` and the GC context-reuse changes have
  landed on top of untested. Either add a Windows build job or read that file
  as dated.
