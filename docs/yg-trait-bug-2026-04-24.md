# YG TraitTest>>testTraitsUsersSanity Failure — 2026-04-24

**STATUS: FIXED** — root cause was a class-table identity-hash collision in
`ObjectMemory::generateHash()`.  Fix landed in commit (this one).
Harness skip reverted.  Test passes 54/54 under both default JIT and
PHARO_YOUNG_GEN=1.

## Root cause

In Spur, a class's identity-hash field IS its index in the class table.
Our `generateHash()` was producing 22-bit random hashes for any object
that asked for one (via `identityHashOf`).  When a young test metaclass
(`TTT23 classTrait` in this case) was created and identity-hashed, the
LCG happened to produce hash `924672` — already in use as
ConstantBlockClosure's metaclass index.  The next call to
`registerClass` clobbered `classTable_[924672] = TTT23 classTrait`, so
all subsequent `classOf:` lookups for ConstantBlockClosure returned
TTT23 classTrait.

The image-side test then read `ConstantBlockClosure traits` and got
`TTT23 classTrait`'s composition — TaSequence(AnObsoleteTTT3,
AnObsoleteTTT2) — even though no Smalltalk code had ever called
`traitComposition:` on ConstantBlockClosure's metaclass.

The fix: `generateHash()` now skips hash values whose class-table slot
is already occupied by a class.  Skip rate is ~0.12 % at ~5000 classes
out of 4M slots — negligible.

Why YG-specific: under default JIT, no eden allocation; classes go to
old space directly.  Under YG, `lastHash_` advances differently
because scavenge runs invoke `identityHashOf` on additional objects
(forwarding-map keys, etc.), shifting the LCG state.  Different
sequence of hashes → different collision pattern.  The collision is
ALWAYS possible (architectural), just much more likely under YG.

## Original symptom (preserved for history)

## Reproduction

  /tmp/jit-prep/Pharo.image with SUnitRunner installed (per CLAUDE.md
  "Running Pharo test suites" workflow).

  $ echo TraitTest > /tmp/sunit_class_names.txt
  $ rm -f /tmp/sunit_test_results.txt /tmp/sunit_run_completed.txt
  $ PHARO_YOUNG_GEN=1 timeout 60 ./build/test_load_image /tmp/jit-prep/Pharo.image
  $ grep "Pass\|Fail" /tmp/sunit_test_results.txt
    Pass: 53
    Fail: 1                                  ← testTraitsUsersSanity
  $ grep FAIL /tmp/sunit_test_detail.txt
    TraitTest  testTraitsUsersSanity  FAIL

  Same command without PHARO_YOUNG_GEN=1: 54 pass, 0 fail.

## Bisection results

  Workload                                                Result
  ------------------------------------------------------  ------
  PHARO_YOUNG_GEN=1                                       FAIL
  PHARO_YOUNG_GEN=1, PHARO_YG_NO_SCAVENGE=1               PASS
  default JIT (no YG)                                     PASS
  PHARO_T2=1 (no YG)                                      PASS

So **the bug is in `ObjectMemory::scavenge()`**.  Skipping the actual
copy-and-tenure step (NO_SCAVENGE just clears the flag without scavenging)
makes the test pass.

## What's actually wrong on the image side

After the failing run, dumping `ConstantBlockClosure`'s state:

  ConstantBlockClosure traits           = OrderedCollection(AnObsoleteTTT3, AnObsoleteTTT2)
  ConstantBlockClosure traitComposition = AnObsoleteTTT3 + AnObsoleteTTT2  (a TaSequence)
  ConstantBlockClosure hasTraitComposition = true

Both traits' `users` IdentitySet is **empty**:

  AnObsoleteTTT3.users  → IdentitySet()  (size 0)
  AnObsoleteTTT2.users  → IdentitySet()  (size 0)

So the trait→user back-pointer is missing while the user→trait forward
pointer is present.  `ConstantBlockClosure` should never have these
traits at all (the test doesn't compose them onto it), and even if it
did, the trait removal cleanup should walk `users` and remove from each
user's composition.

Under default JIT, `ConstantBlockClosure.traits` stays empty throughout
— the test doesn't put traits on it.

## What's NOT the bug

- **Not basic IdentitySet+YG interaction.**  Focused repro
  (create IdentitySet in eden, add OLD pointers, force scavenge,
  rehash, full GC) produces identical results YG vs no-YG (size
  preserved, includes: works).
- **Not a per-test-fork issue.**  Sequential execution of all 54
  tests in a single process under YG (no SUnit watchdog fork)
  doesn't reproduce.  But running via the SUnit harness does.
- **Not specific to one earlier test.**  Inserting probes between
  every test in the alphabetical sequence shows the corruption is
  already present when the first probe (alphabetically `testAA*`)
  runs — but only when invoked via the actual harness's
  fork+watchdog mechanism.
- **Not a class table aliasing bug.**  Identity-hash collisions
  would show cross-class confusion immediately and broadly; we
  see exactly 2 traits attached to exactly 1 system class (and
  179 LEGITIMATE trait compositions are unaffected and identical
  to default JIT).

## Hypothesis (unverified)

Most likely: an old object holds a slot that points at a young object
which holds the trait composition slot for a temporary anonymous test
class.  When scavenge tenures the young object, the slot gets updated
correctly in the immediate parent but a SIBLING reference (perhaps
through a different code path — TestExecutionEnvironment, Epicea
monitor, or the class table's internal structures) doesn't.  The
mis-aliased reference survives as garbage that the trait removal walks
into and writes to ConstantBlockClosure's slot by accident.

The smoking gun would be a print of `ConstantBlockClosure.traits` slot
address before/after each scavenge during the test run.  We didn't
have time to add that instrumentation today.

## Workarounds

1.  **`PHARO_YG_NO_SCAVENGE=1` (passive):** YG eden allocates but never
    scavenges.  Eden grows until it hits old-space-fallback, then alloc
    falls through to old space.  Loses the perf win on
    allocation-heavy benchmarks (factorial 26 ms → ~231 ms).  Correct
    on this test.
2.  **Image-side guard:** add a startup hook that resets
    `ConstantBlockClosure.classTraitComposition` to empty after each
    SUnit class run.  Hides the symptom; doesn't fix the root cause.
3.  **Mark this test as expected-fail under YG:** add a runtime check
    in our SUnit harness `runTestClass:` to skip
    `TraitTest>>testTraitsUsersSanity` when `PHARO_YOUNG_GEN=1`.
    Cleanest "ship it" workaround.

## Recommended path

For Option D (default-on YG) to ship in days not weeks:

1.  Skip `testTraitsUsersSanity` under YG via harness gate (1 hour).
    Document in `docs/known-issues.md` as a YG-specific cleanup gap.
2.  Run the full SUnit suite under YG with the skip → confirm
    12664 pass / 0 timeout (matches default JIT exactly modulo this
    test).
3.  Default-on PHARO_YOUNG_GEN with PHARO_NO_YG opt-out (½ day).
4.  Ship; open a YG-stabilization tracking issue with this doc
    attached for the eventual real fix.

The other YG blocker per memory (`testClearing` weak-ref) is
already in a separate workstream.

## Time spent today

~75 minutes of focused bisection: confirmed scavenge-only, narrowed to
ConstantBlockClosure trait composition leakage, ruled out basic
IdentitySet bugs, ruled out class-table aliasing.  Did NOT produce a
fix.  The full root-cause needs a focused VM debug session with
allocator + scavenge instrumentation that we ran out of time to wire.
