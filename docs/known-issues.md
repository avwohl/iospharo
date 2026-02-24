# Known Issues

Last updated: 2026-02-24

## Not Our Bugs

### Upstream Pharo Test Failures
These fail on the official Pharo VM too:
- WriteBarrier `doubleAt:put:` (Pharo issue #10053, since 2021)
- Missing ephemeron support in finalization tests

### External Package Gaps
- `GArcTest`/`GEllipseTest` failures from missing `#intersectionsWithEllipse:`

### Test Suite Flakiness
- `TestExecutionEnvironmentTest>>testHandleForkedProcessesByAllServices`
  fails in full suite, passes in isolation

## Status
Zero VM-specific test failures — all non-passing tests also fail on
the official Pharo VM.

GUI display, menus, and interaction visually verified working (2026-02-24).
