# Known Issues

Last updated: 2026-08-09

## iOS-Specific

- Taskbar selected button text (e.g. "Welcome") has slight rendering artifacts
- VM thread sleeps forever after interpret() returns (prevents pthread TSD crash)
- VM cannot be re-launched after quit without restarting the process

## VM Defects Found But Not Yet Fixed

Found while clearing the clang 21 warnings (2026-08-09). Each was diagnosed and
deliberately left alone because fixing it changes VM semantics and wants a
decision rather than a drive-by edit. Ordered most serious first.

  1. ObjectMemory::fullGC ignores the result of planCompactSavingForwarders().
     That function returns false when the eden scratch space for saved first
     fields overflows, bailing out with only some objects forwarded. fullGC then
     unconditionally runs updatePointersAfterCompact() and copyAndUnmark() over
     the partial plan, which corrupts the heap. Either the caller must loop
     until planning succeeds, or fullGC must abort the compaction.
     ObjectMemory.cpp, fullGC (~line 1376).

  2. ObjectMemory::sweepGC overcounts overflow objects by 8 bytes. It computes
     `fullSize = hasOverflow ? (objSize + 8) : objSize` where objSize is
     obj->totalSize(), but ObjectHeader::totalSize() (ObjectHeader.hpp:333-342)
     already adds sizeof(uint64_t) for overflow objects. lastLiveEnd therefore
     points 8 bytes into the following object, and oldSpaceFree_ can leave 8
     stale bytes above the last live object that a later scan may read as a
     header. Cross-check: ObjectScanner::next() advances 8 + realSlots*8, which
     confirms totalSize() already includes the overflow word.
     ObjectMemory.cpp, sweepGC (~line 1282).

  3. primitiveObjectPointsTo (prim 132) still has no guard for the 16-bit
     formats Indexable16..Indexable16_3 (12-15, e.g. DoubleByteArray and 16-bit
     strings). They are non-pointer objects that still fall through to the
     pointer-slot scan — the identical defect that was fixed for the 32/64-bit
     formats. Suggested fix is to widen the check to formats 9-23 and drop the
     now-redundant separate byte-array check just above it.

  4. primitiveSetDisplayMode (prim 233) returns Success without doing anything.
     The image is told the display mode changed when it did not. If iOS cannot
     honour the request, failing to Smalltalk would be more truthful — as
     written it is the silent-swallow pattern CLAUDE.md forbids.

  5. primitiveWarpBits (prim 299) cannot apply a colour map. It now fails
     rather than silently producing unmapped output, so this is no longer a
     correctness bug, but the mapped warp path is still unimplemented in the VM
     and falls back to Smalltalk.

  6. The fatal crash dump in Interpreter::pushFrame prints `lastGCStep=%llu`
     with a hardcoded 0. No lastGCStep counter exists anywhere in the codebase,
     so the field is permanently misleading. Either wire it to the step number
     at the last GC, or drop it from the format string.
     Interpreter.cpp (~line 5445).

  7. primitiveStringEncode (531) and primitiveStringDecode (532) are stubs that
     always fail to Smalltalk. A legitimate fallback rather than a bug, but the
     VM-side fast path does not exist.

## Build Toolchain

322 compiler warnings remain, all of them in VMMaker-generated plugin sources.
They are not suppressed. See `docs/vmmaker-issues.md` for the root-cause
analysis, which traces four of the five findings to Slang's inliner in
pharo-project/pharo-vm, and one to a genuine signed/unsigned defect in DSA's
big-integer division.

A full CMake build also requires `Frameworks/` to be populated by
`scripts/build-third-party.sh`. That directory is gitignored, so on a fresh
checkout `cmake --build build` fails on a missing `ffi.h` until the third-party
xcframeworks have been built once.

## Image Bugs We Patch via startup.st

See `docs/image_issues.md` for full details and workarounds.
See `docs/upstream-proposals.md` for proposed upstream fixes.

  1. MicGitHubRessourceReference >> githubApi — nil token causes KeyNotFound
  2. MicDocumentBrowserModel >> document — sends #message instead of #messageText
  3. MicDocumentBrowserPresenter >> childrenOf: — missing outer error handler
  4. Menu shortcut symbols render as "?" — embedded font too old (v2.020)
  5. WarpBlt >> mixPix: drops alpha channel — Smalltalk fallback only averages RGB
  6. Doc browser bullets render as "?" — same font issue as #4

## Upstream Test Bugs (not our problem, not patched)

  7. DebugPointTest >> testTranscriptDebugPoint — fails on all VMs (missing Transcript clear + headless incompatible)

## Test Status

28,071 tests across 2,046 classes. Zero VM-specific failures.
See `docs/test-results.md` for full breakdown.
