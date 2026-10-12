# arm64 full SUnit sweep, resumed-method internal J2J ON (2026-10-12)

Build: `jit` after `1009f274` plus the extension-prefix fixes, the chain-loop
process-switch guard and `PHARO_T1_RESUME_INTERNAL_J2J=1` (now the arm64
default).  Same prepped image and sweep settings as
`sweep-arm-2026-10-11.md`.

    run                        pass    fail  error  wall
    2026-10-11 baseline        27872   13    1      1 h 53 m
    this run                   27876   12    1      56 m

No damaged batch.  Batch 651-700, which wedged (#23) in both earlier full
sweeps, completed this time.  One clean pass does not show #23 is fixed.

Against the baseline's failing selectors (`failing-selectors.txt` here):

    gone   StDebuggerActionModelTest>>testEventAfterProceed
           StSpotterModelTest>>testSpotterModelShouldWaitToPerformActualSearch
           ZnHTTPSTest>>testTransfersSingleClient (TIMEOUT)
    new    MCSmalltalkhubRepositoryTest>>testVersionFromFileNamed (TIMEOUT)
           StObjectTranscriptPresenterTest>>testSmoke

Both new names are intermittent.  The Smalltalkhub test needs the network
and errs or times out alone on the previous default build too (1 of 2).
`testSmoke` errs alone in 1 of 8 runs on this build and 0 of 8 on the
previous default -- too few to call; watch it.
