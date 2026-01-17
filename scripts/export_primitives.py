#!/usr/bin/env python3
"""
Export primitive table from PrimitiveTableExporter.st

This script parses the Smalltalk literal array in PrimitiveTableExporter.st
and generates the same output that the Smalltalk version would produce.
"""

import json
import re
import sys
from datetime import datetime
from pathlib import Path

def parse_primitive_table_spec(st_file: Path) -> list:
    """Parse the primitiveTableSpec method from the Smalltalk file."""
    content = st_file.read_text()

    # Find the primitiveTableSpec method and extract the literal array
    match = re.search(r'primitiveTableSpec\s*\[\s*"[^"]*"\s*\^\s*#\((.*?)\)\s*\]',
                      content, re.DOTALL)
    if not match:
        raise ValueError("Could not find primitiveTableSpec in file")

    array_content = match.group(1)

    entries = []
    category = "Unknown"

    # Parse line by line
    lines = array_content.split('\n')
    for line in lines:
        line = line.strip()
        if not line:
            continue

        # Check for category comment: "Category Name"
        cat_match = re.match(r'^"([^"]+)"', line)
        if cat_match:
            category = cat_match.group(1)
            continue

        # Check for primitive spec: (num name) or (startNum endNum name)
        spec_match = re.match(r'\((\d+)\s+(\d+\s+)?(\w+|nil|0)\)', line)
        if spec_match:
            start_num = int(spec_match.group(1))
            if spec_match.group(2):
                end_num = int(spec_match.group(2).strip())
                name = spec_match.group(3)
            else:
                end_num = start_num
                name = spec_match.group(3)

            # Determine status
            if name in ('nil', '0', 'primitiveFail'):
                status = 'unimplemented'
            elif name.isdigit():
                status = 'quickPrimitive'
            else:
                status = 'implemented'

            # Add entries for range
            for num in range(start_num, end_num + 1):
                entries.append({
                    'num': num,
                    'name': name,
                    'category': category,
                    'status': status
                })

    return entries

# Primitives not yet implemented in C++ - map to nullptr
UNIMPLEMENTED_PRIMITIVES = {
    'primitiveLargeIntegerRem',  # 20 - not commonly used
    # primitiveObjectAt (68) and primitiveObjectAtPut (69) - now implemented
    'primitiveStoreStackp',  # 76 - stack manipulation
    'primitiveMoveToPermSpace',  # 90 - perm space
    'primitiveMoveToPermSpaceInBulk',  # 91
    'primitiveIsInPermSpace',  # 92
    'primitiveMoveToPermSpaceAllOldObjects',  # 93
    'primitiveFullClosureValueWithArgs',  # 208
    'primitiveContextXray',  # 213
    'primitiveVoidVMState',  # 214
    'primitiveMethodXray',  # 216
    'primitiveMethodProfilingData',  # 217
    'primitiveDoNamedPrimitiveWithArgs',  # 218
    'primitiveFormat',  # 231
    'primitiveSignalAtUTCMicroseconds',  # 242
    'primitiveUpdateTimezone',  # 243
    'primitiveUtcAndTimezoneOffset',  # 244
    'primitiveCoarseUTCMicrosecondClock',  # 245
    'primitiveCoarseLocalMicrosecondClock',  # 246
    'primitiveClearVMProfile',  # 250
    'primitiveControlVMProfiling',  # 251
    'primitiveVMProfileSamplesInto',  # 252
    'primitiveCollectCogCodeConstituents',  # 253
    'primitiveFlushExternalPrimitives',  # 570
    'primitiveUnloadModule',  # 571
    'primitiveListBuiltinModule',  # 572
    'primitiveListExternalModule',  # 573
    'primitiveFloat64ArrayAdd',  # 574
    'primitiveNewOldSpace',  # 596
    'primitiveNewWithArgOldSpace',  # 597
    'primitiveNewPinned',  # 598
    'primitiveNewWithArgPinned',  # 599
    # All FFI byte access primitives (600-659) - need FFI support
    'primitiveLoadBoolean8FromBytes', 'primitiveLoadUInt8FromBytes',
    'primitiveLoadInt8FromBytes', 'primitiveLoadUInt16FromBytes',
    'primitiveLoadInt16FromBytes', 'primitiveLoadUInt32FromBytes',
    'primitiveLoadInt32FromBytes', 'primitiveLoadUInt64FromBytes',
    'primitiveLoadInt64FromBytes', 'primitiveLoadPointerFromBytes',
    'primitiveLoadChar8FromBytes', 'primitiveLoadChar16FromBytes',
    'primitiveLoadChar32FromBytes', 'primitiveLoadFloat32FromBytes',
    'primitiveLoadFloat64FromBytes', 'primitiveStoreBoolean8IntoBytes',
    'primitiveStoreUInt8IntoBytes', 'primitiveStoreInt8IntoBytes',
    'primitiveStoreUInt16IntoBytes', 'primitiveStoreInt16IntoBytes',
    'primitiveStoreUInt32IntoBytes', 'primitiveStoreInt32IntoBytes',
    'primitiveStoreUInt64IntoBytes', 'primitiveStoreInt64IntoBytes',
    'primitiveStorePointerIntoBytes', 'primitiveStoreChar8IntoBytes',
    'primitiveStoreChar16IntoBytes', 'primitiveStoreChar32IntoBytes',
    'primitiveStoreFloat32IntoBytes', 'primitiveStoreFloat64IntoBytes',
    'primitiveLoadBoolean8FromExternalAddress', 'primitiveLoadUInt8FromExternalAddress',
    'primitiveLoadInt8FromExternalAddress', 'primitiveLoadUInt16FromExternalAddress',
    'primitiveLoadInt16FromExternalAddress', 'primitiveLoadUInt32FromExternalAddress',
    'primitiveLoadInt32FromExternalAddress', 'primitiveLoadUInt64FromExternalAddress',
    'primitiveLoadInt64FromExternalAddress', 'primitiveLoadPointerFromExternalAddress',
    'primitiveLoadChar8FromExternalAddress', 'primitiveLoadChar16FromExternalAddress',
    'primitiveLoadChar32FromExternalAddress', 'primitiveLoadFloat32FromExternalAddress',
    'primitiveLoadFloat64FromExternalAddress', 'primitiveStoreBoolean8IntoExternalAddress',
    'primitiveStoreUInt8IntoExternalAddress', 'primitiveStoreInt8IntoExternalAddress',
    'primitiveStoreUInt16IntoExternalAddress', 'primitiveStoreInt16IntoExternalAddress',
    'primitiveStoreUInt32IntoExternalAddress', 'primitiveStoreInt32IntoExternalAddress',
    'primitiveStoreUInt64IntoExternalAddress', 'primitiveStoreInt64IntoExternalAddress',
    'primitiveStorePointerIntoExternalAddress', 'primitiveStoreChar8IntoExternalAddress',
    'primitiveStoreChar16IntoExternalAddress', 'primitiveStoreChar32IntoExternalAddress',
    'primitiveStoreFloat32IntoExternalAddress', 'primitiveStoreFloat64IntoExternalAddress',
    # More unimplemented (use VMMaker names, not C++ names!)
    'primitiveRemLargeIntegers',  # 20 - VMMaker name
    'primitiveVoidVMStateForMethod',  # 215 - VMMaker name (maps to primitiveVoidVMState)
    'primitiveArrayBecomeOneWayNoCopyHash',  # 248
    'primitiveArrayBecomeOneWayCopyHash',  # 249
    # SmallFloat primitives - all unimplemented (use VMMaker names)
    'primitiveSmallFloatAdd', 'primitiveSmallFloatSubtract',
    'primitiveSmallFloatLessThan', 'primitiveSmallFloatGreaterThan',
    'primitiveSmallFloatLessOrEqual', 'primitiveSmallFloatGreaterOrEqual',
    'primitiveSmallFloatEqual', 'primitiveSmallFloatNotEqual',
    'primitiveSmallFloatMultiply', 'primitiveSmallFloatDivide',
    'primitiveSmallFloatTruncated', 'primitiveSmallFloatFractionalPart',
    'primitiveSmallFloatExponent', 'primitiveSmallFloatTimesTwoPower',
    'primitiveSmallFloatSquareRoot', 'primitiveSmallFloatSine',  # VMMaker uses "Sine" not "Sin"
    'primitiveSmallFloatArctan', 'primitiveSmallFloatLogN', 'primitiveSmallFloatExp',  # VMMaker uses "LogN" not "Ln"
}

# C++ now uses VMMaker names directly - no mapping needed!

def vmmaker_to_cpp_name(vmmaker_name: str) -> str | None:
    """Return the VMMaker primitive name for C++ use.
    Returns None if the primitive is not yet implemented."""
    if vmmaker_name in UNIMPLEMENTED_PRIMITIVES:
        return None
    return vmmaker_name

def export_json(entries: list, output_path: Path):
    """Export entries as JSON."""
    data = {
        'generated': datetime.now().isoformat(),
        'source': 'VMMaker StackInterpreter initializePrimitiveTable',
        'primitives': entries
    }
    output_path.write_text(json.dumps(data, indent=2))
    print(f"Exported {len(entries)} primitives to {output_path}")

def export_cpp(entries: list, output_path: Path):
    """Export entries as C++ include file."""
    lines = [
        '// Generated primitive table - DO NOT EDIT',
        '// Source: VMMaker StackInterpreter initializePrimitiveTable',
        f'// Generated: {datetime.now().isoformat()}',
        '',
        '// Include this file in Interpreter::initializePrimitives()',
        '// Usage: #include "../ios/generated_primitives.inc"',
        '',
    ]

    implemented_count = 0
    for entry in entries:
        num = entry['num']
        name = entry['name']
        status = entry['status']
        category = entry['category']

        if status in ('unimplemented', 'quickPrimitive'):
            cpp_name = 'nullptr'
        else:
            # Map VMMaker name to C++ name
            cpp_method = vmmaker_to_cpp_name(name)
            if cpp_method is None:
                cpp_name = 'nullptr'  # Not yet implemented in C++
            else:
                cpp_name = f'&Interpreter::{cpp_method}'
                implemented_count += 1

        lines.append(f'primitiveTable_[{num}] = {cpp_name};  // {category}')

    lines.extend([
        '',
        f'// Total: {len(entries)} entries',
        f'// Implemented: {implemented_count}',
    ])

    output_path.write_text('\n'.join(lines))
    print(f"Exported C++ primitive table to {output_path}")
    print(f"  {len(entries)} total, {implemented_count} implemented")

def main():
    script_dir = Path(__file__).parent
    st_file = script_dir / 'PrimitiveTableExporter.st'
    output_dir = script_dir.parent / 'src' / 'ios'

    if not st_file.exists():
        print(f"Error: {st_file} not found")
        sys.exit(1)

    print(f"Parsing {st_file}...")
    entries = parse_primitive_table_spec(st_file)

    # Sort by primitive number
    entries.sort(key=lambda e: e['num'])

    # Export both formats
    export_json(entries, output_dir / 'primitives.json')
    export_cpp(entries, output_dir / 'generated_primitives.inc')

    # Print summary by category
    print("\nPrimitives by category:")
    categories = {}
    for e in entries:
        cat = e['category']
        if cat not in categories:
            categories[cat] = {'total': 0, 'implemented': 0}
        categories[cat]['total'] += 1
        if e['status'] == 'implemented':
            categories[cat]['implemented'] += 1

    for cat, counts in categories.items():
        print(f"  {cat}: {counts['implemented']}/{counts['total']}")

if __name__ == '__main__':
    main()
