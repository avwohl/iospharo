# Test Suite Gaps & Expansion Plan

Bugs that shipped without being caught by our test suites, and Pharo
packages with test suites we should be running but aren't yet.

Last updated: 2026-03-09

For current test results see `docs/test-results.md`.

---

## Bugs Found by Users, Not Tests

These bugs were reported by users or discovered during development.
They were NOT caught by any automated test suite we run.

### WarpBlt quad edge interpolation (Build 62)

primitiveWarpBits (primitive 299) had the quad corner scan order wrong.
Left edge interpolated p1->p4 (horizontal) instead of p1->p2 (vertical),
rotating all WarpBlt output 90 degrees.

Affected: window thumbnails, asFormOfSize:, any scaled/rotated morph.
NOT affected: normal BitBlt rendering (menus, text, windows).

Why tests missed it:
  - No WarpBltTest class in our 577-class SUnit suite
  - Standard BitBltTest only tests primitiveCopyBits, not primitiveWarpBits
  - Pharo does have WarpBlt tests in Graphics-Tests but we don't run that package
  - The Smalltalk fallback for WarpBlt (used when primitive fails) has its
    own bugs (Bug 5 in image_issues.md) so even if we did run the tests,
    they might pass via the fallback while the primitive was broken

Fix: src/vm/Primitives.cpp — corrected interpolation from
p1->p4/p2->p3 to p1->p2/p4->p3.

### WarpBlt alpha channel loss (Build 59)

The Smalltalk fallback for WarpBlt (mixPix:sourceMap:destMap:) only
averaged R,G,B channels — alpha was dropped (always 0). Color inspector
swatches rendered as gray/transparent instead of colored.

Why tests missed it:
  - Same as above — no WarpBlt or Graphics-Tests in our suite
  - Color inspector rendering is visual-only verification

Fix: startup.st patches mixPix: to include alpha. Also implemented
primitiveWarpBits in C++ so the fallback is rarely reached.

### Bullet chars in doc browser (Build 62)

U+2022 BULLET renders as "?" because embedded Source Sans Pro v2.020
doesn't have that glyph. Same root cause as menu shortcut symbols.

Why tests missed it:
  - Doc browser rendering is visual-only
  - No Microdown rendering tests in our suite
  - The font glyph coverage gap is an image issue, not a VM bug

Fix: startup.st patches MicRichTextComposer bulletForLevel: to use
ASCII * and - instead of Unicode bullet.

### Menu shortcut symbols (Build 33)

U+2318 (Cmd), U+2325 (Opt), U+2303 (Ctrl), U+21E7 (Shift) all render
as "?" for the same embedded-font reason.

Why tests missed it:
  - Menu rendering is visual-only
  - No test exercises KMShortcutPrinter output with actual font rendering

Fix: startup.st replaces Unicode symbols with ASCII text in
KMShortcutPrinter symbolTable.

---

## What We Test Today

Full suite: 28,071 tests across 2,046 classes (98.00% pass, zero VM-specific failures).
Higher-level packages: 7,974 tests across 6 packages (99.8% pass).
See `docs/test-results.md` and `docs/higher_level_tests.md`.

---

## Packages We Should Add

These are Pharo packages with real test suites that exercise areas
where we have had bugs or have stubbed implementations.

### Priority 1: Graphics and Rendering

Graphics-Tests (in base image)
  - Includes WarpBltTest, BitBltTest, FormTest, ColorTest
  - Would have caught both WarpBlt bugs above
  - Already in the image — just need to add to run_sunit_tests.st
  - Risk: some tests may assume Cairo/Athens which we stub

Morphic-Tests (in base image)
  - Tests Morph rendering, layout, event handling
  - asFormOfSize: (the thumbnail path) is exercised here
  - Already in the image

### Priority 2: Microdown and Documentation

Microdown-Tests (in base image)
  - Tests the Microdown parser and rich-text composer
  - Would catch bullet rendering issues at the Smalltalk level
    (the "?" rendering requires font-aware testing, but at least
    exercises the code path)
  - MicRichTextComposerTest covers bulletForLevel:, formatting, etc.

Microdown-RichTextComposer-Tests (in base image)
  - Higher-level rendering tests
  - Tests the doc browser model and presenter

### Priority 3: Network and SSL (official Pharo packages)

Network-Tests (in base image)
  - Official Pharo network test suite
  - We have custom VM integration tests but not the official ones
  - May include tests we haven't thought to write

Zinc-HTTP (Metacello loadable)
  - HTTP client/server tests
  - Exercises sockets, SSL, streaming, chunked transfer
  - Large suite (~500+ tests)
  - Load: Metacello new baseline: 'ZincHTTPComponents';
      repository: 'github://svenvc/zinc/repository'; load.

### Priority 4: FFI Stress Tests

UFFI-Tests (in base image)
  - We run 23 FFI test classes already
  - But there may be additional UFFI test classes not in our list
  - Audit: compare our FFI test class list against all TestCase
    subclasses with "FFI" or "UFFI" in their name

### Priority 5: Other Gaps

Compression-Tests (in base image)
  - ZLib, GZip streams
  - MiscPrimitivePlugin compress/decompress

Regex-Tests (in base image)
  - Regular expression engine
  - Pure Smalltalk but exercises string/collection internals

Collections-Tests (in base image)
  - May include tests beyond what Kernel-Tests covers

Spec2-Tests (in base image)
  - UI framework tests
  - May require display interaction

---

## Stubbed Libraries and Their Test Impact

Libraries we stub (return 0 or error) rather than implement.
These represent permanent test gaps unless the stubs are replaced.

### Cairo — All calls return 0

    Status:    No real library bundled
    Stub:      On-demand zero-return lambda in lookupFunction()
    Impact:    Athens canvas silently produces no output
    Packages:  Roassal (visualization), Athens-*, any Cairo-based rendering
    Tests:     Cannot run meaningfully — would all fail
    Plan:      Cairo xcframeworks ARE built but not linked on Mac Catalyst.
               Linking them would make Athens work and unlock Roassal.

### libgit2 — 3 functions stubbed, rest return -1

    Status:    Real library preferred (dlsym first)
    Stub:      git_libgit2_init returns 0, shutdown no-op, version 0.0.0
    Impact:    Iceberg (Pharo's Git integration) non-functional without real lib
    Packages:  Iceberg, IceGitHubAPI, all Git-based package loading
    Tests:     Cannot run — all Git operations would fail
    Plan:      libgit2 xcframework exists in Frameworks/. May already work
               on Mac Catalyst if the xcframework is linked. Needs testing.

### UnixOSProcessPlugin — Not included (iOS prohibition)

    Status:    Absent — no stubs
    Impact:    fork()/exec()/pipe() unavailable
    Packages:  OSProcess, OSSubprocess
    Tests:     Cannot run on iOS, ever (App Store policy)
    Plan:      None — iOS architectural limitation

### Cairo-dependent rendering chain

    libcairo -> libpixman -> libpng (all built but not linked on Catalyst)
    Linking these would enable:
      - Athens canvas (Roassal visualizations)
      - Cairo-based PDF export
      - Potentially better text rendering (Cairo + FreeType + HarfBuzz)

---

## How to Discover More Test Suites

To scan a fresh Pharo image for all available test packages:

    "Print all TestCase subclasses grouped by package"
    (TestCase allSubclasses
        reject: [:c | c isAbstract])
        groupedBy: [:c | c package name]
        thenCollect: [:classes | classes size]
        into: (OrderedDictionary new)

Or from the command line with the reference VM:

    pharo /tmp/Pharo.image eval "
        | packages |
        packages := (TestCase allSubclasses
            reject: [:c | c isAbstract])
            groupedBy: [:c | c package name].
        packages associations
            sort: [:a :b | a value size > b value size].
        packages associations do: [:assoc |
            Transcript show: assoc value size printString, ' tests in ', assoc key; cr].
        Transcript contents"

This would give us a complete inventory of testable packages in the
stock image, ranked by test count.

---

## Running New Package Tests

For packages already in the base image, add their test classes to
scripts/run_sunit_tests.st in the appropriate tier. Graphics-Tests
goes in the visual/rendering tier.

For Metacello-loadable packages, add them to scripts/run_package_tests.sh
following the existing pattern (fresh image, load via reference VM,
run with our VM).

Example — adding Graphics-Tests:

    # In run_sunit_tests.st, add to an appropriate tier:
    #('WarpBltTest' 'FormTest' 'ColorTest' 'BitBltClipboardTest')

Example — adding Zinc-HTTP to run_package_tests.sh:

    declare -A PACKAGES
    PACKAGES[ZincHTTP]="Metacello new baseline: 'ZincHTTPComponents';
        repository: 'github://svenvc/zinc/repository'; load"
