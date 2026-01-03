#!/usr/bin/env python3
"""
Revert oop_wrapper calls back to original isOldObject/isYoungObject calls.
The original functions now use range checking instead of mask-based detection.
"""

import re
import sys

def revert_wrapper_calls(content):
    """Revert oop_wrapper function calls to original VMMemoryMap functions."""

    # oop_is_old((uint64_t)(oop)) -> isOldObject(GIV(memoryMap), oop)
    content = re.sub(
        r'oop_is_old\s*\(\s*\(uint64_t\)\s*\(([^)]+)\)\s*\)',
        r'isOldObject(GIV(memoryMap), \1)',
        content
    )

    # oop_is_young((uint64_t)(oop)) -> isYoungObject(GIV(memoryMap), oop)
    content = re.sub(
        r'oop_is_young\s*\(\s*\(uint64_t\)\s*\(([^)]+)\)\s*\)',
        r'isYoungObject(GIV(memoryMap), \1)',
        content
    )

    # oop_is_permanent((uint64_t)(oop)) -> isPermanentObject(GIV(memoryMap), oop)
    content = re.sub(
        r'oop_is_permanent\s*\(\s*\(uint64_t\)\s*\(([^)]+)\)\s*\)',
        r'isPermanentObject(GIV(memoryMap), \1)',
        content
    )

    return content

def main():
    if len(sys.argv) < 2:
        print("Usage: revert-oop-wrapper-calls.py <cointerp.c> [output.c]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else input_file

    with open(input_file, 'r') as f:
        content = f.read()

    # Count before
    old_count = len(re.findall(r'oop_is_(old|young|permanent)\s*\(', content))

    # Apply reversion
    content = revert_wrapper_calls(content)

    # Count after
    new_count = len(re.findall(r'oop_is_(old|young|permanent)\s*\(', content))

    with open(output_file, 'w') as f:
        f.write(content)

    print(f"Reverted {old_count - new_count} wrapper calls")
    print(f"Remaining wrapper calls: {new_count}")
    print(f"Output: {output_file}")

if __name__ == '__main__':
    main()
