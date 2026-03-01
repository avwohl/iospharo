#!/bin/bash
set -euo pipefail
# Launch VMMaker image with Pharo VM for iOS VM simulation debugging

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOSPHARO_DIR="$SCRIPT_DIR/.."

# Try pharo-vm-ios fork first, fall back to pharo-vm
if [ -d "$SCRIPT_DIR/../../pharo-vm-ios" ]; then
    PHARO_VM_DIR="$SCRIPT_DIR/../../pharo-vm-ios"
else
    PHARO_VM_DIR="$SCRIPT_DIR/../../pharo-vm"
fi

VM_EXECUTABLE="$PHARO_VM_DIR/build-gen/build/vmmaker/vm/Contents/MacOS/Pharo"
VMMAKER_IMAGE="$PHARO_VM_DIR/build-gen/build/vmmaker/image/VMMaker.image"

if [ ! -f "$VM_EXECUTABLE" ]; then
    echo "Error: Pharo VM not found at $VM_EXECUTABLE"
    echo "You may need to build VMMaker first. See pharo-vm README."
    exit 1
fi

if [ ! -f "$VMMAKER_IMAGE" ]; then
    echo "Error: VMMaker.image not found at $VMMAKER_IMAGE"
    exit 1
fi

echo "Launching VMMaker..."
echo "  VM: $VM_EXECUTABLE"
echo "  Image: $VMMAKER_IMAGE"
echo "  Using pharo-vm-ios fork: $(basename "$PHARO_VM_DIR")"
echo ""
echo "=== To generate cointerp.cpp for iOS ==="
echo ""
echo "1. Load the Slang-iOS package:"
echo ""
echo "   Metacello new"
echo "       baseline: 'VMMaker';"
echo "       repository: 'tonel://$PHARO_VM_DIR/smalltalksrc';"
echo "       load: #('Slang-iOS')."
echo ""
echo "2. See scripts/generate-cointerp-cpp.md for full instructions"
echo ""

exec "$VM_EXECUTABLE" "$VMMAKER_IMAGE"
