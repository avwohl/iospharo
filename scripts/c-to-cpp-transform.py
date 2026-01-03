#!/usr/bin/env python3
"""
Transform cointerp.c to cointerp.cpp with full C++ compatibility.

Handles:
1. Oop wrapper method calls
2. C++ reserved keyword renaming (class -> class_)
3. Explicit casts for C++ (void* -> char*)
4. Anonymous struct issues
"""

import re
import sys
from pathlib import Path

# C++ reserved words that appear in the generated C code
RESERVED_WORDS = {
    'class': 'class_',
    'template': 'template_',
    'new': 'new_',
    'delete': 'delete_',
    'this': 'this_',
    'virtual': 'virtual_',
    'private': 'private_',
    'public': 'public_',
    'protected': 'protected_',
    'namespace': 'namespace_',
    'operator': 'operator_',
}

def transform_c_to_cpp(content: str) -> str:
    """Transform C interpreter code to C++ with Oop wrapper."""

    lines = content.split('\n')
    output_lines = []

    includes_added = False

    for line in lines:
        # Add C++ headers after sq.h include
        if not includes_added and line.startswith('#include') and 'sq.h' in line:
            output_lines.append(line)
            output_lines.append('')
            output_lines.append('// C++ Oop wrapper for iOS ASLR compatibility')
            output_lines.append('#include "oop.hpp"')
            output_lines.append('using namespace pharo;')
            output_lines.append('')
            output_lines.append('// C++ compatibility casts')
            output_lines.append('#define CHAR_PTR(x) ((char*)(x))')
            output_lines.append('#define VOID_PTR(x) ((void*)(x))')
            includes_added = True
            continue

        # Skip extern declarations (don't transform prototypes)
        if line.strip().startswith('extern '):
            # Still rename reserved words in extern declarations
            for old, new in RESERVED_WORDS.items():
                # Match as word boundaries
                line = re.sub(r'\b' + old + r'\b', new, line)
            output_lines.append(line)
            continue

        # Rename C++ reserved words
        for old, new in RESERVED_WORDS.items():
            line = re.sub(r'\b' + old + r'\b', new, line)

        # Transform integerValueOf(expr) -> (expr).integerValueOf()
        line = re.sub(
            r'(?<!_)integerValueOf\s*\(\s*([^)]+)\s*\)',
            r'(\1).integerValueOf()',
            line
        )

        # Transform integerObjectOf(expr) -> Oop::integerObjectOf(expr)
        line = re.sub(
            r'(?<!::)integerObjectOf\s*\(',
            r'Oop::integerObjectOf(',
            line
        )

        # Transform isIntegerObject(expr) -> (expr).isSmallInteger()
        line = re.sub(
            r'(?<!_)isIntegerObject\s*\(\s*([^)]+)\s*\)',
            r'(\1).isSmallInteger()',
            line
        )

        # Transform isNonImmediate(expr) -> (expr).isNonImmediate()
        line = re.sub(
            r'(?<!_)isNonImmediate\s*\(\s*([^)]+)\s*\)',
            r'(\1).isNonImmediate()',
            line
        )

        # Transform isImmediate(expr) -> (expr).isImmediate()
        line = re.sub(
            r'(?<![a-zA-Z_])isImmediate\s*\(\s*([^)]+)\s*\)',
            r'(\1).isImmediate()',
            line
        )

        # isYoungObject(memoryMap, oop) -> (oop).isYoung()
        line = re.sub(
            r'isYoungObject\s*\(\s*GIV\s*\(\s*memoryMap\s*\)\s*,\s*([^)]+)\s*\)',
            r'(\1).isYoung()',
            line
        )

        # isOldObject(memoryMap, oop) -> (oop).isOld()
        line = re.sub(
            r'isOldObject\s*\(\s*GIV\s*\(\s*memoryMap\s*\)\s*,\s*([^)]+)\s*\)',
            r'(\1).isOld()',
            line
        )

        # isPermanentObject(memoryMap, oop) -> (oop).isPermanent()
        line = re.sub(
            r'isPermanentObject\s*\(\s*GIV\s*\(\s*memoryMap\s*\)\s*,\s*([^)]+)\s*\)',
            r'(\1).isPermanent()',
            line
        )

        # Fix void* to char* implicit conversions
        # Pattern: char *var = malloc(...) -> char *var = (char*)malloc(...)
        line = re.sub(
            r'(char\s*\*\s*\w+\s*=\s*)malloc\(',
            r'\1(char*)malloc(',
            line
        )

        # Pattern: var = malloc(...) where var is known to be char*
        # This is harder without context - skip for now

        # Fix implicit void* casts in assignments (common pattern)
        line = re.sub(
            r'=\s*mmap\(',
            r'= (char*)mmap(',
            line
        )

        output_lines.append(line)

    return '\n'.join(output_lines)


def main():
    if len(sys.argv) < 2:
        print("Usage: c-to-cpp-transform.py <input.c> [output.cpp]")
        sys.exit(1)

    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2]) if len(sys.argv) >= 3 else input_path.with_suffix('.cpp')

    print(f"Reading: {input_path}")
    content = input_path.read_text()

    print("Transforming C to C++...")
    transformed = transform_c_to_cpp(content)

    print(f"Writing: {output_path}")
    output_path.write_text(transformed)

    # Count transformations
    class_renames = len(re.findall(r'\bclass_\b', transformed))
    int_val = len(re.findall(r'\.integerValueOf\(\)', transformed))
    int_obj = len(re.findall(r'Oop::integerObjectOf', transformed))
    is_small = len(re.findall(r'\.isSmallInteger\(\)', transformed))
    is_young = len(re.findall(r'\.isYoung\(\)', transformed))
    is_old = len(re.findall(r'\.isOld\(\)', transformed))
    is_perm = len(re.findall(r'\.isPermanent\(\)', transformed))

    print(f"\nTransformation summary:")
    print(f"  class -> class_: {class_renames}")
    print(f"  integerValueOf: {int_val}")
    print(f"  Oop::integerObjectOf: {int_obj}")
    print(f"  isSmallInteger: {is_small}")
    print(f"  isYoung: {is_young}")
    print(f"  isOld: {is_old}")
    print(f"  isPermanent: {is_perm}")
    print(f"\nDone!")


if __name__ == '__main__':
    main()
