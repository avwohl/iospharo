# Scripts

## Build
- `build-libffi.sh` — Cross-compile libffi as xcframework (iOS, Simulator, Mac Catalyst)
- `build-sdl2.sh` — Cross-compile SDL2 as xcframework
- `build-third-party.sh` — Build cairo, freetype, harfbuzz, pixman, libpng, OpenSSL, libssh2, libgit2

## Test Running
- `build-ffi-test-library.sh` — builds `libTestLibrary.dylib`, the C library the
  TFFI test classes call into. The Pharo VM distribution ships it and we do not,
  and without it `TFCallbacksTest` raises `SymbolNotFoundError` from inside a
  callback, the callback never answers, and the VM spins — which reads as a hang.
  With it, that class runs in about 0.4s. Needs a pharo-vm checkout; set
  `PHARO_VM_SRC` if yours is not at `~/esrc/pharo-vm`.
- `pharo-headless-test/` — Submodule: headless test runner + fake GUI (https://github.com/avwohl/pharo-headless-test)
- `run_batch_tests.sh` — Shell wrapper that runs tests in batches of 50 classes
- `run_regression_tests.st` — Regression test runner
- `run_callback_suite.st` — FFI callback test suite
- `run_callback_tests.st` — FFI callback tests
- `time_tests.st` — Per-test timing for performance analysis
- `test-mac-catalyst.sh` — Build and test the Mac Catalyst app

## VM Behaviour Tests (SUnit classes, filed into an image)

Each defines a `TestCase` subclass covering a primitive the stock Pharo suite
does not reach. Once filed in they are also picked up by the dynamic-discovery
pass in `pharo-headless-test/run_sunit_tests.st`.

- `test_bitblt_depth.st` — `primitiveCopyBits` across source/dest depths
- `test_bitblt_translucent.st` — combination rules 30/31 and `copyBitsTranslucent:`
- `test_bitblt_lowdepth.st` — depth-8 destinations keep their pixel order and
  their colours. Compares colours, not pixel values: a depth change also applies
  a palette map, so equal indices are the wrong thing to assert.
- `test_warpblt.st` — `primitiveWarpBits`: quad orientation, smoothing, alpha,
  colour map, clipping. Pharo 13.1 ships no `WarpBltTest` of its own.
- `test_pointsto.st` — `primitiveObjectPointsTo`: raw-data formats must not be
  scanned as pointer slots. The guard has been wrong twice, so each format
  class gets a case that constructs the false positive deliberately rather than
  hoping to trip over it.
- `test_bitblt_depth_matrix.st` — all 36 ordered depth pairs, plus regression
  cases for the conversion bugs fixed on 2026-08-17. Asserts position
  independence — a pixel must convert the same in a form as it does alone —
  rather than naming expected colours, which would just encode today's behaviour.
  The source form is 33 wide by 5 rows on purpose: an even width hides a 16bpp
  stride error exactly, and row 0 lands correctly however wrong the stride is, so
  a single row cannot show one. 9/9 here; 4/9 before that day's fixes, with five
  of the nine failing as hard errors because 15 of the 36 pairs did not convert
  at all.
- `test_deep_stack_unwind.st` — unwind and handler search must not give up on a
  deep stack. Primitives 195 and 197 stopped after 10000 contexts and answered
  nil, which callers read as "there is no unwind context" and "there is no
  handler". Ensure blocks were skipped, exceptions went unhandled, and
  `Process>>terminate` killed its target and never returned. Time-guarded.
  Passes on this branch, which raised the limit long ago; kept as a guard.
- `test_nonlocal_return.st` — a `^` in a block must return from the activation
  that created it, not merely from some activation of the same method. Two live
  activations of one method are common (`valueWithExit` is `^self value: [^nil]`,
  so nesting it puts two on the stack), and getting this wrong makes the outer
  exit silently not exit. Time-guarded, because the failure mode is a hang.
- `test_directory_enumeration.st` — directory listings must not include `.` and
  `..`. Filtering them is the plugin's job; returning them made the image treat
  `..` as a real child, so `deleteAll` and any other tree walk climbed toward the
  filesystem root and never came back. On a VM without the fix this class cannot
  complete at all, because its own tearDown is a `deleteAll`.
- `test_traits.st`, `test_network.st`, `test_menu_items.st`

Run one against our VM:

    cp /tmp/Pharo.image /tmp/t.image
    ./build/test_load_image /tmp/t.image eval "
      | res |
      '$(pwd)/scripts/test_warpblt.st' asFileReference fileIn.
      res := (Smalltalk at: #WarpBltTest) suite run.
      '/tmp/result.txt' asFileReference writeStreamDo: [:f |
        f nextPutAll: res printString ]"
    cat /tmp/result.txt

Two things that will otherwise waste your time: temporaries have to be
declared at the very top, before the `fileIn`; and naming `WarpBltTest`
directly is a compile error, because the whole argument is compiled before any
of it runs and the class does not exist yet. Reach it through `Smalltalk at:`.

## Build / Primitive Tooling
- `PrimitiveTableExporter.st` — Exports primitive table from VMMaker to JSON/C++
- `export_primitives.py` — Python wrapper for PrimitiveTableExporter

## Image Preparation
- `prepare_image.st` — Prepare a Pharo image for testing
- `simple_startup.st` — Minimal startup test script
- `SimpleFormWorldRenderer.st` — Fallback form renderer for headless mode

## iOS Driver
- `create_ios_driver.st` — Create OSiOSDriver class in image
- `install_ios_driver.st` — Install OSiOSDriver as active driver
- `load_ios_driver.st` — Load OSiOSDriver from file

## Diagnostics
- `ios_diagnostics.st` — iOS-specific diagnostic helpers
- `test_menu_items.st` — Test menu item rendering
- `debug_startup.st` — Debug startup sequence
- `launch-vmmaker.sh` — Launch VMMaker simulation environment
