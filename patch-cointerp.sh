#!/bin/bash
# Patch cointerp.c to skip strict address checks for iOS
# iOS uses ASLR and can't guarantee specific memory addresses

COINTERP="$1"

if [ ! -f "$COINTERP" ]; then
    echo "Usage: $0 <path-to-cointerp.c>"
    exit 1
fi

echo "Patching $COINTERP for iOS..."

# Create backup
cp "$COINTERP" "${COINTERP}.orig"

# Remove the address check error calls
# These patterns match lines like:
#   if (!((xxx->yyyStart)) == ADDRESS)) { logError(...); error("Error allocating"); }
# We want to remove the error call but keep the logError as a warning

sed -i '' '
    # Pattern for address checks followed by error calls
    # Match: if (!((xxx) == ADDRESS)) { logError(...); error(...); }
    # Replace error with logWarn and remove the abort

    # codeZone check
    /Could not allocate codeZone in the expected place/,+1 {
        s/error("Error allocating");/logWarn("iOS: Proceeding with different address");/
    }

    # newSpace check
    /Could not allocate newSpace in the expected place/,+1 {
        s/error("Error allocating");/logWarn("iOS: Proceeding with different address");/
    }

    # oldSpace check
    /Could not allocate oldSpace in the expected place/,+1 {
        s/error("Error allocating");/logWarn("iOS: Proceeding with different address");/
    }

    # permSpace check
    /Could not allocate permSpace in the expected place/,+1 {
        s/error("Error allocating");/logWarn("iOS: Proceeding with different address");/
    }

    # stack check
    /Could not allocate stack in the expected place/,+1 {
        s/error("Error allocating");/logWarn("iOS: Proceeding with different address");/
    }
' "$COINTERP"

echo "Patched successfully!"
