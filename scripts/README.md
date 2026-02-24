# Scripts

## Test Running
- `run_sunit_tests.st` — Main SUnit test runner, injected into fresh images via fileIn
- `run_batch_tests.sh` — Shell wrapper that runs tests in batches of 50 classes
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

## VMMaker / Reference
- `launch-vmmaker.sh` — Launch VMMaker simulation environment
- `debug_startup.st` — Debug startup sequence
