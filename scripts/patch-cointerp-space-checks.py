#!/usr/bin/env python3
"""
Patch cointerp.c to use oop_wrapper space check functions.

Transforms:
  isOldObject(GIV(memoryMap), oop) -> oop_is_old((uint64_t)(oop))
  isYoungObject(GIV(memoryMap), oop) -> oop_is_young((uint64_t)(oop))
  isPermanentObject(GIV(memoryMap), oop) -> oop_is_permanent((uint64_t)(oop))
"""

import re
import sys

def patch_space_checks(content):
    """Replace space check function calls with oop_wrapper equivalents."""

    # Pattern to match: isXxxObject(GIV(memoryMap), anything)
    # The 'anything' can be a simple variable, GIV(xxx), or complex expression

    # isOldObject(GIV(memoryMap), oop) -> oop_is_old((uint64_t)(oop))
    content = re.sub(
        r'isOldObject\s*\(\s*GIV\s*\(\s*memoryMap\s*\)\s*,\s*([^)]+)\)',
        r'oop_is_old((uint64_t)(\1))',
        content
    )

    # isYoungObject(GIV(memoryMap), oop) -> oop_is_young((uint64_t)(oop))
    content = re.sub(
        r'isYoungObject\s*\(\s*GIV\s*\(\s*memoryMap\s*\)\s*,\s*([^)]+)\)',
        r'oop_is_young((uint64_t)(\1))',
        content
    )

    # isPermanentObject(GIV(memoryMap), oop) -> oop_is_permanent((uint64_t)(oop))
    content = re.sub(
        r'isPermanentObject\s*\(\s*GIV\s*\(\s*memoryMap\s*\)\s*,\s*([^)]+)\)',
        r'oop_is_permanent((uint64_t)(\1))',
        content
    )

    # Also handle getMemoryMap() variant
    # isOldObject(getMemoryMap(), oop) -> oop_is_old((uint64_t)(oop))
    content = re.sub(
        r'isOldObject\s*\(\s*getMemoryMap\s*\(\s*\)\s*,\s*([^)]+)\)',
        r'oop_is_old((uint64_t)(\1))',
        content
    )

    # isYoungObject(getMemoryMap(), oop) -> oop_is_young((uint64_t)(oop))
    content = re.sub(
        r'isYoungObject\s*\(\s*getMemoryMap\s*\(\s*\)\s*,\s*([^)]+)\)',
        r'oop_is_young((uint64_t)(\1))',
        content
    )

    # isPermanentObject(getMemoryMap(), oop) -> oop_is_permanent((uint64_t)(oop))
    content = re.sub(
        r'isPermanentObject\s*\(\s*getMemoryMap\s*\(\s*\)\s*,\s*([^)]+)\)',
        r'oop_is_permanent((uint64_t)(\1))',
        content
    )

    return content

def add_include(content):
    """Add oop_wrapper.h include after other includes."""
    # Find a good place to insert - after the last #include
    include_pattern = r'(#include\s+[<"][^>"]+[>"])'
    includes = list(re.finditer(include_pattern, content))

    if includes:
        last_include = includes[-1]
        pos = last_include.end()

        # Check if oop_wrapper.h is already included
        if 'oop_wrapper.h' not in content:
            # Add include after the last existing include
            content = content[:pos] + '\n\n/* iOS ASLR-compatible space detection */\n#if PHARO_IOS_OOP_WRAPPER\n#include "oop_wrapper.h"\n#endif' + content[pos:]

    return content

def remove_extern_decls(content):
    """Remove the extern declarations for old space check functions (when using wrapper)."""
    # Wrap the extern declarations in #ifndef PHARO_IOS_OOP_WRAPPER
    patterns = [
        r'extern sqInt isOldObject\([^;]+;',
        r'extern sqInt isYoungObject\([^;]+;',
        r'extern sqInt isPermanentObject\([^;]+;',
    ]

    for pattern in patterns:
        # Wrap each with #ifndef
        content = re.sub(
            pattern,
            r'#if !PHARO_IOS_OOP_WRAPPER\n\g<0>\n#endif',
            content
        )

    return content

def main():
    if len(sys.argv) < 2:
        print("Usage: patch-cointerp-space-checks.py <cointerp.c> [output.c]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else input_file

    with open(input_file, 'r') as f:
        content = f.read()

    # Count before
    old_count = len(re.findall(r'is(Old|Young|Permanent)Object\s*\(\s*GIV\s*\(\s*memoryMap\s*\)', content))

    # Apply transformations
    content = add_include(content)
    content = patch_space_checks(content)

    # Count after
    new_count = len(re.findall(r'is(Old|Young|Permanent)Object\s*\(\s*GIV\s*\(\s*memoryMap\s*\)', content))

    with open(output_file, 'w') as f:
        f.write(content)

    print(f"Patched {old_count - new_count} space check calls")
    print(f"Remaining: {new_count}")
    print(f"Output: {output_file}")

if __name__ == '__main__':
    main()
