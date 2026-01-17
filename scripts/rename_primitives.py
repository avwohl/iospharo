#!/usr/bin/env python3
"""Rename C++ primitive methods to match VMMaker names."""

import re
from pathlib import Path

# Mapping: VMMaker name -> current C++ name
# We want to rename C++ to VMMaker, so we reverse this
CPP_TO_VMMAKER = {
    # LargeInteger primitives
    'primitiveLargeIntegerAdd': 'primitiveAddLargeIntegers',
    'primitiveLargeIntegerSubtract': 'primitiveSubtractLargeIntegers',
    'primitiveLargeIntegerLessThan': 'primitiveLessThanLargeIntegers',
    'primitiveLargeIntegerGreaterThan': 'primitiveGreaterThanLargeIntegers',
    'primitiveLargeIntegerLessOrEqual': 'primitiveLessOrEqualLargeIntegers',
    'primitiveLargeIntegerGreaterOrEqual': 'primitiveGreaterOrEqualLargeIntegers',
    'primitiveLargeIntegerEqual': 'primitiveEqualLargeIntegers',
    'primitiveLargeIntegerNotEqual': 'primitiveNotEqualLargeIntegers',
    'primitiveLargeIntegerMultiply': 'primitiveMultiplyLargeIntegers',
    'primitiveLargeIntegerDivide': 'primitiveDivideLargeIntegers',
    'primitiveLargeIntegerMod': 'primitiveModLargeIntegers',
    'primitiveLargeIntegerDiv': 'primitiveDivLargeIntegers',
    'primitiveLargeIntegerQuo': 'primitiveQuoLargeIntegers',
    'primitiveLargeIntegerBitAnd': 'primitiveBitAndLargeIntegers',
    'primitiveLargeIntegerBitOr': 'primitiveBitOrLargeIntegers',
    'primitiveLargeIntegerBitXor': 'primitiveBitXorLargeIntegers',
    'primitiveLargeIntegerBitShift': 'primitiveBitShiftLargeIntegers',

    # Float primitives
    'primitiveFloatTruncated': 'primitiveTruncated',
    'primitiveFloatSquareRoot': 'primitiveSquareRoot',
    'primitiveFloatSin': 'primitiveSine',
    'primitiveFloatArctan': 'primitiveArctan',
    'primitiveFloatLn': 'primitiveLogN',
    'primitiveFloatExp': 'primitiveExp',

    # Other renames
    'primitiveGrowMemory': 'primitiveGrowMemoryByAtLeast',
    'primitiveSetIdentityHash': 'primitiveSetOrHasIdentityHash',
    'primitiveCompareWith': 'primitiveStringCompareWith',
}

def rename_in_file(filepath: Path, dry_run: bool = False) -> int:
    """Rename primitives in a file. Returns count of replacements."""
    content = filepath.read_text()
    original = content
    count = 0

    for old_name, new_name in CPP_TO_VMMAKER.items():
        # Count occurrences
        occurrences = content.count(old_name)
        if occurrences > 0:
            count += occurrences
            print(f"  {old_name} -> {new_name} ({occurrences} occurrences)")
            if not dry_run:
                content = content.replace(old_name, new_name)

    if not dry_run and content != original:
        filepath.write_text(content)

    return count

def main():
    import sys
    dry_run = '--dry-run' in sys.argv

    if dry_run:
        print("DRY RUN - no changes will be made\n")

    base = Path('/Users/wohl/src/iospharo/src/vm')
    files = [
        base / 'Interpreter.cpp',
        base / 'Interpreter.hpp',
        base / 'Primitives.cpp',
    ]

    total = 0
    for f in files:
        if f.exists():
            print(f"\n{f.name}:")
            count = rename_in_file(f, dry_run)
            total += count
            if count == 0:
                print("  (no changes)")

    print(f"\nTotal: {total} replacements")
    if dry_run:
        print("\nRun without --dry-run to apply changes")

if __name__ == '__main__':
    main()
