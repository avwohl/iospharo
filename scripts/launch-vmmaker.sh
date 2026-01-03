#!/bin/bash
# Launch VMMaker image with Pharo VM for iOS VM simulation debugging

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHARO_VM_DIR="$SCRIPT_DIR/../../pharo-vm"

VM_EXECUTABLE="$PHARO_VM_DIR/build-gen/build/vmmaker/vm/Contents/MacOS/Pharo"
VMMAKER_IMAGE="$PHARO_VM_DIR/build-gen/build/vmmaker/image/VMMaker.image"

if [ ! -f "$VM_EXECUTABLE" ]; then
    echo "Error: Pharo VM not found at $VM_EXECUTABLE"
    exit 1
fi

if [ ! -f "$VMMAKER_IMAGE" ]; then
    echo "Error: VMMaker.image not found at $VMMAKER_IMAGE"
    exit 1
fi

echo "Launching VMMaker..."
echo "  VM: $VM_EXECUTABLE"
echo "  Image: $VMMAKER_IMAGE"
echo ""
echo "Once Pharo opens, evaluate the following in a Playground:"
echo ""
echo "  \"Load iOS simulator setup\""
echo "  (FileStream fileNamed: '$SCRIPT_DIR/ios-simulator-setup.st') fileIn."
echo ""

exec "$VM_EXECUTABLE" "$VMMAKER_IMAGE"
