#!/usr/bin/env bash
# build-roassal-catalog-image.sh — RUNS ON AN x86 LINUX BOX.
# Builds a Roassal-loaded "catalog" image to reproduce the ARM context-storm.
#
# WHY x86: Roassal3's Cairo FFI segfaults stock Cog on macOS, and there is no
# Pharo-13 stock Cog VM for ARM64 Linux. But Smalltalk images are
# ARCHITECTURE-NEUTRAL: build the image here (x86 Linux, apt cairo works), then
# scp it to a macOS-ARM machine and run it with the custom ARM VM
# (build/test_load_image) — the storm's native environment.
#
# After building: prep the image with the custom VM locally
#   test_load_image cat.image eval "'scripts/pharo-headless-test/setup_fake_gui.st' asFileReference fileIn.
#     'scripts/pharo-headless-test/run_sunit_tests.st' asFileReference fileIn. Smalltalk saveAs: 'catprep'"
# (stock Cog HANGS on the Roassal image; the custom VM opens it fine.)
# Then run the RS/Debugger/Morph subset in RUNNER mode, A/B with/without
# PHARO_NO_BECOME_FORWARDER, under PHARO_HEAP_CENSUS=1 — watch for the Context
# explosion (storm) vs bounded heap.
set -uo pipefail
sudo apt-get update -qq && sudo apt-get install -y -qq libcairo2-dev libfreetype6-dev libfontconfig1-dev
mkdir -p ~/catbuild && cd ~/catbuild
[ -x ./pharo ] || curl -sL https://get.pharo.org/64/vm130 | bash >/dev/null 2>&1
[ -f ./Pharo.image ] || curl -sL https://get.pharo.org/64/130 | bash >/dev/null 2>&1
# Roassal3 for Pharo 13 is on the 'Pharo13' branch of pharo-graphics/Roassal:
timeout 1500 ./pharo Pharo.image eval --save \
  "[Metacello new baseline: 'Roassal3'; repository: 'github://pharo-graphics/Roassal:Pharo13/src'; onConflict: [:e | e useIncoming]; load. 'RS-LOADED'] on: Error do: [:e | 'RS-ERR: ', e messageText]"
./pharo Pharo.image eval "(Smalltalk globals includesKey: #RSCanvas) printString, ' RStests=', ((TestCase allSubclasses reject: [:c | c isAbstract]) count: [:c | c name beginsWith: 'RS']) printString"
echo "catalog image: ~/catbuild/Pharo.image (scp to macOS-ARM, run with custom VM)"
