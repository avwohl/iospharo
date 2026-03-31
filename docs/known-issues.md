# Known Issues

Last updated: 2026-03-31

## iOS-Specific

- Taskbar selected button text (e.g. "Welcome") has slight rendering artifacts
- VM thread sleeps forever after interpret() returns (prevents pthread TSD crash)
- VM cannot be re-launched after quit without restarting the process

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
