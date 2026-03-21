# Scripts

## Build
- `build-libffi.sh` — Cross-compile libffi as xcframework (iOS, Simulator, Mac Catalyst)
- `build-sdl2.sh` — Cross-compile SDL2 as xcframework
- `build-third-party.sh` — Build cairo, freetype, harfbuzz, pixman, libpng, OpenSSL, libssh2, libgit2

## Test Running
- `pharo-headless-test/` — Submodule: headless test runner + fake GUI (https://github.com/avwohl/pharo-headless-test)
- `run_batch_tests.sh` — Shell wrapper that runs tests in batches of 50 classes
- `run_regression_tests.st` — Regression test runner
- `run_callback_suite.st` — FFI callback test suite
- `run_callback_tests.st` — FFI callback tests
- `time_tests.st` — Per-test timing for performance analysis
- `test-mac-catalyst.sh` — Build and test the Mac Catalyst app

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
