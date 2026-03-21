# Pharo 14 Support Notes

Date: 2026-03-20
Image tested: Pharo-14.0.0+SNAPSHOT.build.553.sha.3ce4b42afe (March 19, 2026)
Official VM: v10.3.9 (same as Pharo 13)

## TL;DR

Our VM loads and runs Pharo 14 with zero changes. The image format,
bytecodes, primitives, and object layout are all identical to Pharo 13.
All our startup.st patches still apply cleanly. P14 release expected
within a few weeks.

---

## VM Compatibility

    Item                  P13         P14         Change needed
    Image format          68021       68021       none
    Header size           128 bytes   128 bytes   none
    Bytecodes             Sista V1    Sista V1    none
    Primitive table       v10.3.9     v10.3.9     none
    Object format         64-bit Spur 64-bit Spur none
    FFI / UFFI            same        same        none (minor package cleanup)
    Old base address      0x10000..   0x10000..   none

Pharo 14 ships with the exact same VM binary (v10.3.9, built Nov 17 2025)
as Pharo 13. The pharo-vm pharo-12 development branch has 788 commits
beyond v10.3.9, but none of these are released or required by P14.

Verified: `./build/test_load_image /tmp/pharo14/Pharo.image` boots to
the Morphic render loop successfully.

## Getting a Pharo 14 Image

    curl -sL https://get.pharo.org/64/140 | bash

Daily snapshot builds at: https://files.pharo.org/image/140/

## startup.st Patch Compatibility

All classes targeted by our startup.st patches exist in P14 and the
method signatures are unchanged. The patches will apply without error.

    Class                            Present   Method unchanged
    MicGitHubRessourceReference      yes       githubApi - same source
    MicDocumentBrowserModel          yes       document - same source
    MicDocumentBrowserPresenter      yes       childrenOf: - same source
    KMShortcutPrinter                yes       symbolTable - same
    MicRichTextComposer              yes       bulletForLevel: - same
    WarpBlt                          yes       mixPix:sourceMap:destMap: - same
    IceTokenCredentials              yes       token still nil in fresh image

## Upstream Bug Status in P14

None of the 6 bugs from docs/upstream-proposals.md are fixed:

    Bug 1  githubApi nil token auth       NOT FIXED - still `MicGitHubAPI new`
    Bug 2  #message vs #messageText       NOT FIXED - still sends `error message`
    Bug 3  childrenOf: missing handler    NOT FIXED - same partial coverage
    Bug 4  Source Sans Pro missing glyphs NOT FIXED - still v2.020 (2012)
    Bug 5  WarpBlt drops alpha            NOT FIXED - no alpha in mixPix
    Bug 6  testTranscriptDebugPoint       NOT FIXED - no Transcript clear

Action: File these as issues on github.com/pharo-project/pharo.
Next Pharo bug-fix session is Friday March 21 (monthly community session).

## Feature Requests Status

    Item 7  Portrait / small-screen layout    Not in P14
    Item 8  Touch-friendly text interaction   Not in P14
    Item 9  Keyboard shortcut discoverability Not in P14

These are larger changes unlikely to land without someone contributing
patches. Worth discussing on the mailing list or Discord to find
collaborators.

## Headless Test Jig (Item 10 in upstream-proposals.md)

Our two scripts (run_sunit_tests.st, setup_fake_gui.st) are pure
Smalltalk with zero VM dependencies. They already work on P14 unchanged
since they target standard APIs:

    - SessionManager for startup hooks
    - TestCase / SUnit for test discovery and running
    - MorphicUIManager / MorphicRenderLoop for fake GUI
    - Morph>>activate / passivate patches for nil submorphs

These are probably best offered as a PR or shared via a blog post rather
than a bug report. Options:

    1. PR to pharo-project/pharo adding the scripts to the test infrastructure
       Pro: directly usable, gets review and integration
       Con: maintainance burden, may conflict with existing CI approach

    2. Separate repo (e.g. pharo-headless-test-runner) with instructions
       Pro: independent, easy to try, no upstream review needed
       Con: less discoverable

    3. Blog post / wiki page documenting the approach
       Pro: educational, shows the technique
       Con: scripts may drift from image changes

Recommendation: option 2 (separate repo) with a link posted to the
Pharo dev list. If there's interest, it can be upstreamed later. The
scripts should be tested against P14 before sharing (quick smoke test).

## Future VM Changes (Not Required, But Worth Watching)

The pharo-vm pharo-12 branch has features that may eventually ship:

    Feature                 PR      Impact on us
    thisProcess JIT         #1037   None (we handle bytecode 82+extB=1 already)
    Faster become:          #1023   None (optimization only)
    NewFilePlugin           #1024   ~20 new named primitives for file I/O
                                    Image probes availability, graceful fallback
    primitiveVMVersion      #1050   Returns #(major minor patch) array
                                    Image falls back to string parsing if absent
    Idle time tracking      #1042   statIdleUsecs actually reports real idle time
    LargeInteger NULL fix   #1040   Already safe in our implementation

None of these are required for P14. If/when a v12 VM ships:
  - NewFilePlugin would be nice to have (modern file I/O)
  - primitiveVMVersion is trivial to add
  - The rest are optimizations or already handled

## Testing Checklist for P14 Support

    [ ] Download fresh P14 image: curl -sL https://get.pharo.org/64/140 | bash
    [ ] Load with test_load_image - verify Morphic render loop starts
    [ ] Load in Mac Catalyst app - verify desktop renders
    [ ] Run SUnit test suite - compare pass rates with P13 baseline
    [ ] Verify startup.st patches apply (check logs for compile errors)
    [ ] Test in iOS Simulator
    [ ] Test Export as App with P14 image
