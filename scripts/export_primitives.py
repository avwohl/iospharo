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
        '// Include this file in Interpreter::initializePrimitiveTable()',
        '// Usage: #include "generated_primitives.inc"',
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
            cpp_name = f'&Interpreter::{name}'
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
