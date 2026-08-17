#!/bin/bash
# Build libTestLibrary.dylib, the FFI test-support library.
#
# TFCallbacksTest, TFCalloutTest and their neighbours call C functions —
# shortCallout, singleCallToCallback, and a pile of type-marshalling helpers —
# out of a library the Pharo VM distribution ships and we do not. Without it the
# image raises SymbolNotFoundError from inside a callback, the callback never
# answers, and the VM spins until its callback timeout. That reads as a hang, so
# TFCallbacksTest sat on the skip list as one. With this library present the
# whole class runs in about 0.4 seconds.
#
# The sources live in the pharo-vm checkout, not here. Point PHARO_VM_SRC at it
# if yours is somewhere other than the default.
set -euo pipefail

PHARO_VM_SRC="${PHARO_VM_SRC:-$HOME/esrc/pharo-vm}"
SRC_DIR="$PHARO_VM_SRC/ffiTestLibrary"
OUT_DIR="${1:-$(cd "$(dirname "$0")/.." && pwd)/build}"

if [ ! -d "$SRC_DIR" ]; then
    echo "error: no ffiTestLibrary at $SRC_DIR" >&2
    echo "       set PHARO_VM_SRC to your pharo-vm checkout, e.g." >&2
    echo "       PHARO_VM_SRC=~/src/pharo-vm $0" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/libTestLibrary.dylib"

clang -dynamiclib -O1 -arch "$(uname -m)" \
      -I"$SRC_DIR/includes" \
      -o "$OUT" \
      "$SRC_DIR"/src/*.c

echo "built $OUT"

# Fail loudly if the two symbols the callback tests need did not make it in,
# rather than leaving a library that loads and then cannot resolve anything.
for sym in _shortCallout _singleCallToCallback; do
    if ! nm -gU "$OUT" | grep -q " $sym\$"; then
        echo "error: $OUT is missing $sym" >&2
        exit 1
    fi
done
echo "symbols ok: shortCallout, singleCallToCallback"
echo
echo "The image looks the library up by plain name, so run tests with:"
echo "    DYLD_LIBRARY_PATH=$OUT_DIR ./build/test_load_image <image>"
