# System-Time: Duration>>asDelay rounds up to the next millisecond

Local data (not part of the PR text)

- Upstream repo: pharo-project/pharo, base branch Pharo15
- Local branch: pr15/duration-asdelay-rounds-up
- Work tree: /Users/wohl/pharo-work/wt/pharo-duration-asdelay-rounds-up
- Commit: b223409d93 System-Time: Duration>>asDelay rounds up to the next millisecond
- Related issues: #15852 (open, "Delay forDuration: dummy uses"): about another defect, not this one
- Source in this project: scripts/image-fixes/32-duration-wait-rounds-up.st (fix and test DurationTest>>testAsDelayDoesNotRoundDown)

## Pull request text

Duration>>asDelay rounds down to whole milliseconds, so Duration>>wait can return before the Duration has passed. This change makes asDelay round up. A negative Duration gives the same Delay as before.

Related: #15852

### The defect

Duration>>asDelay makes a Delay for "self asMilliSeconds" milliseconds. Duration>>asMilliSeconds rounds down. Duration>>wait uses asDelay. A wait for 99.98 milliseconds therefore makes a Delay of 99 milliseconds, and the wait ends 0.98 milliseconds early.

Reproduction in a clean Pharo 15 image with the stock VM:

```smalltalk
(Duration nanoSeconds: 99980000) asDelay millisecondDelayDuration
```

Observed: 99. Expected: 100.

The contract of Duration>>wait is that the wait does not end before the Duration has passed. The comment of Delay class>>forMilliseconds: says that a Delay lasts "approximately" the time given. That word is about a late end, not about an early end.

The defect is hard to see on the stock VM, because the stock VM ends a Delay 1 to 6 milliseconds late on the test machine. On a VM with accurate timers, a wait is visibly short. A task that repeats each 100 milliseconds for 300 milliseconds then gets a fourth run before the 300 milliseconds end (TkThreadTest>>testPeriodFixedDuration, in the PharoMisc Tasks package, not in this repository).

### The cause

Duration>>asDelay is "^ Delay forMilliseconds: self asMilliSeconds". Duration>>asMilliSeconds uses "//", which rounds down.

### The change

Duration>>asDelay takes the nanoseconds of the receiver. A part of a millisecond counts as one more millisecond: (nanoseconds + 999999) // 1000000. A negative Duration keeps the old value (nanoseconds // 1000000), so Delay class>>forMilliseconds: signals the same error as before. Duration>>asMilliSeconds does not change.

Other users of asDelay in Pharo 15 are not hurt:

- Duration>>wait: the wait is now at most 1 millisecond longer and never shorter.
- TKTService (line 103) makes a Delay from stepDelay and waits on the Delay. A longer wait by less than 1 millisecond does not change the logic.
- Fuel-Core-Tests FLDelayedSerializationMock waits for its duration. The same is true.
- DurationTest>>testAsDelay only checks that the Delay is not equal to the Duration.

### Tests

The new test DurationTest>>testAsDelayDoesNotRoundDown checks the Delay length for 99980000 ns (100), 100000000 ns (100), 100000001 ns (101), 1 ns (1) and zero (0). The test does not depend on timer accuracy. The test fails before the change and passes after the change.

The class DurationTest runs as a regression check.

### How it was tested

Image and VM: Pharo 15 build 141 b4d2dfd with the stock Cog VM v12.0.6-beta bebdbaca8, on macOS arm64. The loader of the Tonel files of the branch loaded the change into a clean Pharo 15 image ("DONE 0 errors").

Proof test DurationTest>>testAsDelayDoesNotRoundDown:
- Image with the test only (no fix): FAIL 3 of 3 ("Got 99 instead of 100.")
- Image with the test and the fix: PASS 3 of 3

Regression classes, run on the base image and on the image with the change:
- DurationTest: 71 passed before, 72 passed after (the new test is the one more). No failure.
- DelayTest: 5 passed before and after. No failure.
- TKTServiceTest: 4 passed before and after. No failure.

Not tested: Windows and Linux.

### Notes

- The change affects only Durations with a fractional millisecond. The Delay becomes at most 1 millisecond longer.
- Not tested: Windows and Linux. The change has no platform code.
- The change does not add a test that measures real wait time, because such a test depends on the timers of the VM.

Generated with [Claude Code](https://claude.com/claude-code)
