# Spur: signal low space before old space reaches its limit

## Local data (not part of the PR text)

- Upstream repository: pharo-project/pharo-vm, base branch pharo-12
- Local branch: pr15/spur-low-space-before-limit
- Work tree: /Users/wohl/pharo-work/wt/vm-spur-low-space-before-limit
- Commits:
  - 3efdc417d Spur: signal low space before old space reaches its limit
  - a126b25e0 VMMakerTests: low space is signalled before old space reaches its limit
- Related issue: pharo-project/pharo-vm#918 (OPEN, no linked PR). Also pharo-project/pharo#16900 (maybe related).
- Source in this project: Cog branch fix/low-space-signal-before-old-space-limit, docs/upstream/cog-changes.md, docs/upstream/cog-low-space-before-old-space-limit.patch
- Probe: scripts/cogdiff/lowspace_recursion.st (run with scripts/vmrun.sh eval --old-space 1024)
- Review notes (round 1):
  - Fixed: a refused segment (answer 0 of growOldSpaceByAtLeast:callingOperation:) now counts as "cannot grow", like the answer nil.
  - Fixed: one local variable for the low space test, because the post-scavenge heap size has no second use.
  - Fixed: new test for the garbage case. NOT run, because the review stage forbids a VM run. The validation stage must run the test.
  - Not done: a test for the refused-segment path (the simulation cannot refuse a segment in a simple way) and a test for the case that growth is possible. Only the code review covers the refused-segment path.
  - Kept: the grow loop uses max(growHeadroom, two new spaces). The text now states the exact condition.
- Review notes (round 2):
  - Fixed: the garbage test did not prove the full-GC block (the old code also answers true there). The test is now a guard test, and the .tests file lists the test as a guard, not as a "must fail before" test. The PR text no longer says that the test shows the full GC. A test that tells the two cases apart needs a run to choose heapSizeAtPreviousGC, and the stage forbids a VM run.
  - Fixed: the commit body of the VM change has a line width of 72 columns.
  - Fixed: local variable grown has the type size_t (Slang pragma). The pair "isNil or = 0" stays: in the simulation the answer is nil at the limit, and in C both cases are 0. The comment says so. Slang was not run, so the generated C was not checked.
  - Fixed: the first test asserts that the free old space is above two new spaces before the allocation.
  - Kept "Fixes #918" for the owner to decide: the match is probable (same messages), not proven. Change it to "Related: #918" if the owner does not agree. The Notes say that the workload of the reporter was not reproduced.
- Review notes (round 3): the author rewrote the test. The memory map of the test refuses each segment (no logError: path), the test allocates an old-space chunk, sets heapGrowthToSizeGCRatio and heapSizeAtPreviousGC, and has no guard test. Measured: FAIL on unchanged pharo-12, PASS with the change, 3 runs, 4 variants. Logs: validate/spur-low-space-before-limit/vmtest-*-final.log.
- Open point for the owner: the threshold of two new spaces is a design choice. If the old-space limit is very small, the VM signals low space at once.

## Pull request text

Signal low space before old space reaches its limit

Fixes #918 (probable match, see Notes)

This change makes `sufficientSpaceAfterGC:` signal low space early enough when old space cannot grow. Before the change, a runaway allocation with `--maxOldSpaceSize` stopped the VM before the image got the low space signal.

### The defect

When the old-space limit is set (`--maxOldSpaceSize`, or the system refuses a new segment), a process that allocates without end stops the VM. The VM prints `Could not allocate new object in the old space`, and then a segmentation fault occurs in the report code (measured on macOS arm64 only). The image never gets the low space signal, so `OutOfMemory` is never raised.

Reproduction, in a clean Pharo image with the stock VM:

```
pharo --maxOldSpaceSize=1024 Pharo.image eval "
| recurse |
Smalltalk installLowSpaceWatcher.
recurse := nil.
recurse := [ :n | (recurse value: n + 1) + 1 ].
[ recurse value: 1. 'returned' ] on: Error do: [ :e | e class name ]"
```

The symptom in #918 matches: the messages there include `Could not allocate more memory while copying objects to OldSpace during GC`. The code of #918 was not run for this change.

- Observed: no answer, the VM stops.
- Expected: the answer is `OutOfMemory`, and the image goes on.

### The cause

`SpurMemoryManager>>sufficientSpaceAfterGC:` answers false (low space) only when the free old space is below `lowSpaceThreshold`. The image sets that value to 400000 bytes. One scavenge can tenure more than that. Then `SpurGenerationScavenger>>copyToOldSpace:bytes:format:` cannot get the space and stops the VM. The low space check came too late.

### The change

In `sufficientSpaceAfterGC:`, when old space cannot grow, space is low if the free old space is less than two new spaces. One new space is for the next scavenge. One new space is for the low space watcher of the image and the unwind of the process that the watcher stops. The VM also treats a segment that the system refuses as "old space cannot grow".

Before the method gives this answer, the method can run one full GC. The method does this when old space has grown by more than one new space since the last GC. Then garbage in old space does not count as use.

The loop that grows old space now runs while the free old space is less than the larger of `growHeadroom` (16 MB by default) and two new spaces. If two new spaces are more than 16 MB, the VM can grow old space more than before, also when growth is possible. When two new spaces are 16 MB or less, and old space can grow, the behavior does not change.

### Tests

New test: `VMSpurScavengerTest>>testLowSpaceIsSignalledBeforeOldSpaceReachesItsLimit`.

- The test replaces the memory manager of the memory map with an object that refuses each request for a segment. So old space cannot grow, and the limit path with `logError:` is not used (in the simulation, `logError:` signals `Error`).
- The test allocates one old-space object, so that the free old space is a fixed small amount: above `lowSpaceThreshold` and below two new spaces. The test does not depend on the size of old space.
- `sufficientSpaceAfterGC:` must answer false.
- The memory manager of the test counts the requests.
- Without the change, `sufficientSpaceAfterGC:` asks for a segment again and again, because in the simulation the answer 0 of `growOldSpaceByAtLeast:callingOperation:` is not nil. After 20 requests the test signals `TestFailure`, so the test does not hang. So the test fails on the unchanged code because of the repeated requests, not because of a wrong answer.
- The test runs in all four simulator variants of the class (word size 4 and 8, with and without the composed image format). Each run takes about 100 ms.

Regression classes to run: `VMSpurScavengerTest`, `VMSpurOldSpaceGarbageCollectorTest`, `VMSpurOldSpaceTest`.

The image-level probe is the reproduction above.

### How it was tested

Note: the probe results below with the "patched" VM come from an earlier build. That build had an earlier form of this commit, before the review. The results of the final form of the branch are in "Build of this branch alone, and of the set" at the end of this section.

Versions: Pharo 15 build 141 b4d2dfd image. Stock Cog VM v12.0.6-beta bebdbaca8 (cog15). Patched Cog: pharo-12 at 398023142 plus the fix commits of this project (cogp). macOS arm64.

Log files: /Users/wohl/pharo-work/validate/spur-low-space-before-limit/.

Image-level probe (the reproduction above, old-space limit 1024 MB, one run each):

- Stock Cog: no answer. The VM log has `Could not allocate more memory while ensuring sufficient space after GC. MaxOldSpaceSize reached.`, then `Could not allocate new object in the old space`, then `Segmentation fault: 11`.
- Patched Cog: the answer is `OutOfMemory: `, and the image goes on.
- Result: the defect occurs before the change and does not occur after the change.

VMMakerTests (clean VMMaker image of unchanged pharo-12, and an image with the change loaded). The loader log of each image says `DONE 0 errors`. The runs used stock Cog cog15 through `vmrun.sh test`:

- `VMSpurScavengerTest>>testLowSpaceIsSignalledBeforeOldSpaceReachesItsLimit` on the clean image (test only loaded): FAIL in 3 of 3 runs for each of the 4 simulator variants, with the message `old space growth is requested again and again`.
- The same test on the image with the change: PASS in 3 of 3 runs for each of the 4 variants.
- Regression classes on the image with the change: `VMSpurScavengerTest` 168 PASS (the 164 earlier tests and the 4 variants of the new test), `VMSpurOldSpaceGarbageCollectorTest` 132 PASS, `VMSpurOldSpaceTest` 264 PASS. No FAIL, no ERROR.
- Earlier rounds: the first form of the test gave ERROR before and after the change (no `lowSpaceThreshold` getter, `totalFreeOldSpace:` trips `checkFreeSpace:`, `heapGrowthToSizeGCRatio` is nil, and `logError:` signals `Error`). The new test has none of these four causes.

Summary: the VM change works at image level (probe). The new VMMakerTests test fails without the change and passes with the change.

Build of this branch alone, and of the set:

We also built this branch alone (2026-11-23, `scripts/upstream-pr/vm-pr-build.sh`, pharo-12 at 398023142 plus this branch).  The same probe with an old-space limit of 1024 MB: the stock VM stops with "Segmentation fault: 11", and the VM of this branch answers `OutOfMemory: ` and goes on.

The five VM branches of this set together (branch `pr15/all-five`,
built alone) ran the whole Pharo 15 suite with the six image pull
requests of the set: 40369 tests, 40278 pass, 2 fail, 88 skip, 1
expected failure.  The skips are the same as on the stock VM.  The two
failures do not come from this change:
`CodeSimulationTest>>testErrorCodeNotFound` needs another Cog change, and
`ZnClientTest>>testQueryGoogle` depends on the answer of Google and fails
on the stock VM too.

### Notes

- The threshold of two new spaces is a heuristic. A scavenge can in theory tenure up to one whole new space. The second new space is a margin for the unwind. The maintainers can choose another value.
- If the old-space limit is very small, the VM signals low space early.
- The extra full GC adds a pause only when old space is near its limit.
- Generate the VM code again with Slang.
- The workload of the reporter of #918 was not reproduced. The match with #918 is probable, not proven.
- The generated C code of `sufficientSpaceAfterGC:` was not checked. The local variable `grown` has the type `size_t`, so in C the answers nil and 0 are both 0.
- The test covers the case that the system refuses a segment. No test covers the case that growth is possible, or the limit `maxOldSpaceSize` (the simulation signals `Error` there).
- The test does not run the full GC block that the method has before the answer.
- Not tested on Windows and Linux, and not tested with a system that refuses memory when no limit is set.
- Related: pharo-project/pharo#16900.

Generated with [Claude Code](https://claude.com/claude-code)
