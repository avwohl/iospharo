#!/usr/bin/env python3
"""
extract_stencils.py - Extract machine code stencils from compiled object file

Copyright (c) 2026 Aaron Wohl. Licensed under the MIT License.

This script:
1. Compiles stencils.cpp to an object file with Clang -O2
2. Parses the Mach-O object to extract each stencil function's:
   - Machine code bytes
   - Relocation entries (with hole names and types)
3. Generates a C++ header (generated_stencils.hpp) with byte arrays
   and relocation tables suitable for copy-and-patch JIT.

Usage:
    python3 scripts/extract_stencils.py

Output:
    src/vm/jit/generated_stencils.hpp

Requires:
    - Clang (for compiling stencils)
    - Python 3.6+ (no external deps)
"""

import struct
import subprocess
import sys
import os
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple

# ===== CONFIGURATION =====

PROJECT_ROOT = Path(__file__).resolve().parent.parent
STENCIL_SRC = PROJECT_ROOT / "src" / "vm" / "jit" / "stencils" / "stencils.cpp"
OUTPUT_HPP = PROJECT_ROOT / "src" / "vm" / "jit" / "generated_stencils.hpp"
STENCIL_OBJ = Path("/tmp/jit_stencils.o")

CLANG = "clang++"
CFLAGS_COMMON = [
    "-c", "-O2", "-std=c++17",
    "-fno-exceptions", "-fno-rtti",
    "-fno-asynchronous-unwind-tables",
    "-fno-stack-protector",
    "-mllvm", "-hot-cold-split=false",
    "-mllvm", "-enable-machine-outliner=never",
]

CFLAGS_ARM64 = CFLAGS_COMMON + ["-target", "arm64-apple-macos14"]
CFLAGS_X86_64 = CFLAGS_COMMON + ["-target", "x86_64-apple-macos14"]

# ===== MACH-O CONSTANTS =====

MH_MAGIC_64 = 0xFEEDFACF
MH_CIGAM_64 = 0xCFFAEDFE  # byte-swapped
CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_X86_64 = 0x01000007

# Load commands
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYSYMTAB = 0x0B

# Section types
S_REGULAR = 0x00

# N-list types
N_EXT = 0x01
N_UNDF = 0x00
N_SECT = 0x0E

# ARM64 relocation types (from mach-o/arm64/reloc.h)
ARM64_RELOC_UNSIGNED = 0
ARM64_RELOC_SUBTRACTOR = 1
ARM64_RELOC_BRANCH26 = 2
ARM64_RELOC_PAGE21 = 3
ARM64_RELOC_PAGEOFF12 = 4
ARM64_RELOC_GOT_LOAD_PAGE21 = 5
ARM64_RELOC_GOT_LOAD_PAGEOFF12 = 6
ARM64_RELOC_POINTER_TO_GOT = 7
ARM64_RELOC_TLVP_LOAD_PAGE21 = 8
ARM64_RELOC_TLVP_LOAD_PAGEOFF12 = 9
ARM64_RELOC_ADDEND = 10

# x86_64 relocation types (from mach-o/x86_64/reloc.h)
X86_64_RELOC_UNSIGNED = 0
X86_64_RELOC_SIGNED = 1
X86_64_RELOC_BRANCH = 2
X86_64_RELOC_GOT_LOAD = 3
X86_64_RELOC_GOT = 4
X86_64_RELOC_SUBTRACTOR = 5
X86_64_RELOC_SIGNED_1 = 6
X86_64_RELOC_SIGNED_2 = 7
X86_64_RELOC_SIGNED_4 = 8

# ARM64 relocation type names (for the generated header)
ARM64_RELOC_TYPE_MAP = {
    ARM64_RELOC_BRANCH26: "RelocType::ARM64_BRANCH26",
    ARM64_RELOC_PAGE21: "RelocType::ARM64_PAGE21",
    ARM64_RELOC_PAGEOFF12: "RelocType::ARM64_PAGEOFF12",
    ARM64_RELOC_GOT_LOAD_PAGE21: "RelocType::ARM64_GOT_LOAD_PAGE21",
    ARM64_RELOC_GOT_LOAD_PAGEOFF12: "RelocType::ARM64_GOT_LOAD_PAGEOFF12",
}

# x86_64 relocation type names — all map to X86_64_PC32 (RIP-relative 32-bit)
# BRANCH: JMP/CALL rel32 → direct code target
# GOT_LOAD: movq sym@GOTPCREL(%rip), %reg → literal pool entry address
# GOT: movl/movslq sym@GOTPCREL(%rip), %reg → literal pool entry address
X86_64_RELOC_TYPE_MAP = {
    X86_64_RELOC_BRANCH: "RelocType::X86_64_PC32",
    X86_64_RELOC_GOT_LOAD: "RelocType::X86_64_PC32",
    X86_64_RELOC_GOT: "RelocType::X86_64_PC32",
}

# Map hole symbol names to HoleKind enum values
HOLE_KIND_MAP = {
    "__HOLE_CONTINUE": "HoleKind::Continue",
    "__HOLE_BRANCH_TARGET": "HoleKind::BranchTarget",
    "__HOLE_OPERAND": "HoleKind::Operand",
    "__HOLE_OPERAND2": "HoleKind::Operand2",
    "__HOLE_RT_SEND": "HoleKind::RuntimeHelper",
    "__HOLE_RT_RETURN": "HoleKind::RuntimeHelper",
    "__HOLE_RT_ARITH_OVERFLOW": "HoleKind::RuntimeHelper",
    "__HOLE_NIL_OOP": "HoleKind::RuntimeHelper",
    "__HOLE_TRUE_OOP": "HoleKind::RuntimeHelper",
    "__HOLE_FALSE_OOP": "HoleKind::RuntimeHelper",
    "__HOLE_MEGA_CACHE": "HoleKind::RuntimeHelper",
    "__HOLE_RT_PUSH_FRAME": "HoleKind::RuntimeHelper",
    "__HOLE_RT_POP_FRAME": "HoleKind::RuntimeHelper",
    "__HOLE_RT_J2J_CALL": "HoleKind::RuntimeHelper",
    "__HOLE_RT_ARRAY_PRIM": "HoleKind::RuntimeHelper",
    "__HOLE_RESUME_ADDR": "HoleKind::ResumeAddr",
}

# Finer-grained runtime helper IDs for distinguishing helper functions.
# The JIT compiler needs to know WHICH helper to patch in.
RUNTIME_HELPER_ID = {
    "__HOLE_CONTINUE": 0,
    "__HOLE_BRANCH_TARGET": 0,
    "__HOLE_OPERAND": 0,
    "__HOLE_OPERAND2": 0,
    "__HOLE_RT_SEND": 1,
    "__HOLE_RT_RETURN": 2,
    "__HOLE_RT_ARITH_OVERFLOW": 3,
    "__HOLE_NIL_OOP": 4,
    "__HOLE_TRUE_OOP": 5,
    "__HOLE_FALSE_OOP": 6,
    "__HOLE_MEGA_CACHE": 7,
    "__HOLE_RT_PUSH_FRAME": 8,
    "__HOLE_RT_POP_FRAME": 9,
    "__HOLE_RT_J2J_CALL": 10,
    "__HOLE_RT_ARRAY_PRIM": 11,
    "__HOLE_RESUME_ADDR": 0,
}


# ===== DATA CLASSES =====

@dataclass
class Relocation:
    offset: int           # Byte offset within stencil
    reloc_type: int       # Mach-O relocation type
    symbol: str           # Hole symbol name
    addend: int = 0       # Addend from ARM64_RELOC_ADDEND

@dataclass
class StencilInfo:
    name: str             # Function name (without leading _)
    offset: int           # Offset in __text section
    size: int             # Code size in bytes
    code: bytes           # Raw machine code
    relocs: List[Relocation] = field(default_factory=list)


# ===== MACH-O PARSER =====

class MachOParser:
    """Minimal Mach-O 64-bit parser for extracting stencil code + relocations."""

    def __init__(self, data: bytes):
        self.data = data
        self.symbols: List[dict] = []
        self.sections: List[dict] = []
        self.text_section: Optional[dict] = None
        self.string_table: bytes = b""
        self._parse()

    def _parse(self):
        magic = struct.unpack_from("<I", self.data, 0)[0]
        if magic == MH_MAGIC_64:
            self.endian = "<"
        elif magic == MH_CIGAM_64:
            self.endian = ">"
        else:
            raise ValueError(f"Not a Mach-O 64-bit file (magic: 0x{magic:08X})")

        # Mach-O header: magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved
        hdr = struct.unpack_from(f"{self.endian}IIIIIIII", self.data, 0)
        ncmds = hdr[4]

        offset = 32  # Size of mach_header_64
        symtab_off = dysymtab_off = 0

        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from(f"{self.endian}II", self.data, offset)

            if cmd == LC_SEGMENT_64:
                self._parse_segment(offset)
            elif cmd == LC_SYMTAB:
                self._parse_symtab(offset)

            offset += cmdsize

        # Find the __TEXT,__text section
        for sec in self.sections:
            if sec["segname"].rstrip("\0") == "__TEXT" and sec["sectname"].rstrip("\0") == "__text":
                self.text_section = sec
                break

    def _parse_segment(self, offset: int):
        # segment_command_64: cmd, cmdsize, segname(16), vmaddr, vmsize,
        #                     fileoff, filesize, maxprot, initprot, nsects, flags
        e = self.endian
        segname = self.data[offset+8:offset+24].decode("ascii")
        nsects = struct.unpack_from(f"{e}I", self.data, offset + 64)[0]

        sec_offset = offset + 72  # Size of segment_command_64
        for _ in range(nsects):
            sec = self._parse_section(sec_offset)
            self.sections.append(sec)
            sec_offset += 80  # Size of section_64

    def _parse_section(self, offset: int) -> dict:
        e = self.endian
        sectname = self.data[offset:offset+16].decode("ascii")
        segname = self.data[offset+16:offset+32].decode("ascii")
        addr, size, fileoff, align, reloff, nreloc = struct.unpack_from(
            f"{e}QQIIII", self.data, offset + 32)
        flags = struct.unpack_from(f"{e}I", self.data, offset + 64)[0]

        return {
            "sectname": sectname,
            "segname": segname,
            "addr": addr,
            "size": size,
            "fileoff": fileoff,
            "align": align,
            "reloff": reloff,
            "nreloc": nreloc,
            "flags": flags,
        }

    def _parse_symtab(self, offset: int):
        e = self.endian
        _, _, symoff, nsyms, stroff, strsize = struct.unpack_from(
            f"{e}IIIIII", self.data, offset)

        self.string_table = self.data[stroff:stroff + strsize]

        for i in range(nsyms):
            sym_offset = symoff + i * 16  # nlist_64 is 16 bytes
            strx, type_byte, sect, desc, value = struct.unpack_from(
                f"{e}IBBHQ", self.data, sym_offset)

            name = self._get_string(strx)
            self.symbols.append({
                "name": name,
                "type": type_byte,
                "sect": sect,
                "desc": desc,
                "value": value,
            })

    def _get_string(self, offset: int) -> str:
        end = self.string_table.index(b"\0", offset)
        return self.string_table[offset:end].decode("ascii")

    def get_text_code(self) -> bytes:
        """Return the raw bytes of the __text section."""
        sec = self.text_section
        if not sec:
            raise ValueError("No __text section found")
        return self.data[sec["fileoff"]:sec["fileoff"] + sec["size"]]

    def get_relocations(self) -> List[Tuple[int, int, bool, bool, int, str]]:
        """
        Parse relocations for the __text section.
        Returns list of (offset, type, pcrel, extern, length, symbol_name).
        """
        sec = self.text_section
        if not sec or sec["nreloc"] == 0:
            return []

        result = []
        e = self.endian
        pending_addend = 0

        for i in range(sec["nreloc"]):
            roff = sec["reloff"] + i * 8
            r_word0, r_word1 = struct.unpack_from(f"{e}II", self.data, roff)

            r_address = r_word0
            r_symbolnum = r_word1 & 0x00FFFFFF
            r_pcrel = bool((r_word1 >> 24) & 1)
            r_length = (r_word1 >> 25) & 3
            r_extern = bool((r_word1 >> 27) & 1)
            r_type = (r_word1 >> 28) & 0xF

            # Handle ARM64_RELOC_ADDEND: it sets an addend for the next reloc
            if r_type == ARM64_RELOC_ADDEND:
                pending_addend = r_address  # addend is in the address field
                continue

            sym_name = ""
            if r_extern and r_symbolnum < len(self.symbols):
                sym_name = self.symbols[r_symbolnum]["name"]

            result.append({
                "offset": r_address,
                "type": r_type,
                "pcrel": r_pcrel,
                "extern": r_extern,
                "length": r_length,
                "symbol": sym_name,
                "addend": pending_addend,
            })
            pending_addend = 0

        return result


# ===== STENCIL EXTRACTION =====

def compile_stencils(arch: str) -> Path:
    """Compile stencils.cpp to an object file for the given architecture."""
    cflags = CFLAGS_ARM64 if arch == "arm64" else CFLAGS_X86_64
    obj = Path(f"/tmp/jit_stencils_{arch}.o")
    cmd = [CLANG] + cflags + [
        "-I", str(PROJECT_ROOT / "src" / "vm"),
        "-I", str(PROJECT_ROOT / "src" / "vm" / "jit"),
        "-o", str(obj), str(STENCIL_SRC),
    ]
    print(f"  Compiling ({arch}): {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR: Compilation failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    print(f"  Output: {obj} ({obj.stat().st_size} bytes)")
    return obj


# SimStack stencils that use x19/x20 via inline asm (ARM64 only).
# These rely on the compiler NOT using x19/x20 for its own purposes.
# After compilation, we verify this by checking for STP/LDP save/restore.
SIMSTACK_STENCILS = {
    "stencil_flush1", "stencil_flush2",
    "stencil_pushTemp_E", "stencil_pushTemp_1", "stencil_pushTemp_2",
    "stencil_pushRecvVar_E", "stencil_pushRecvVar_1", "stencil_pushRecvVar_2",
    "stencil_pushLitConst_E", "stencil_pushLitConst_1", "stencil_pushLitConst_2",
    "stencil_pushLitVar_E", "stencil_pushLitVar_1", "stencil_pushLitVar_2",
    "stencil_pushReceiver_E", "stencil_pushReceiver_1", "stencil_pushReceiver_2",
    "stencil_pushTrue_E", "stencil_pushTrue_1", "stencil_pushTrue_2",
    "stencil_pushFalse_E", "stencil_pushFalse_1", "stencil_pushFalse_2",
    "stencil_pushNil_E", "stencil_pushNil_1", "stencil_pushNil_2",
    "stencil_pop_2", "stencil_pop_1", "stencil_pop_E",
    "stencil_dup_E", "stencil_dup_1", "stencil_dup_2",
    "stencil_storeTemp_1", "stencil_storeTemp_2",
    "stencil_popStoreTemp_2", "stencil_popStoreTemp_1",
    "stencil_storeRecvVar_1",
    "stencil_popStoreRecvVar_2", "stencil_popStoreRecvVar_1",
    "stencil_addSmallInt_2", "stencil_subSmallInt_2", "stencil_mulSmallInt_2",
    "stencil_lessThanSmallInt_2", "stencil_greaterThanSmallInt_2",
    "stencil_lessEqualSmallInt_2", "stencil_greaterEqualSmallInt_2",
    "stencil_equalSmallInt_2", "stencil_notEqualSmallInt_2",
    "stencil_jumpTrue_1", "stencil_jumpFalse_1",
    "stencil_returnTop_1", "stencil_returnTop_E", "stencil_returnReceiver_1",
}


def verify_simstack_register_safety(stencils: List[StencilInfo], arch: str):
    """Verify that SimStack stencils don't have compiler-generated x19/x20 save/restore.

    The register-based SimStack approach relies on the compiler NOT touching
    x19/x20 in these small tail-call functions. If the compiler generates
    STP/LDP for x19/x20, our inline asm writes would be undone by the restore.
    """
    if arch != "arm64":
        return  # Only relevant for ARM64

    # ARM64 STP/LDP encoding for x19/x20:
    # STP x20, x19, [sp, #imm]! : 0xA9xx4FF4 (pre-indexed) or 0xA90x4FF4
    # LDP x20, x19, [sp], #imm  : 0xA8xx4FF4 (post-indexed)
    # We check for any STP/LDP involving x19 (register 19 = 0x13) or x20 (0x14)
    # in the Rt/Rt2 fields.
    issues = []
    for s in stencils:
        if s.name not in SIMSTACK_STENCILS:
            continue
        code = s.code
        for i in range(0, len(code) - 3, 4):
            insn = struct.unpack_from("<I", code, i)[0]
            # STP/LDP GP 64-bit: bits 31:25 = 1010100 = 0x54
            # This distinguishes from MOV/ORR (0x55) which shares bits 31:26
            is_stp_ldp = ((insn >> 25) & 0x7F) == 0x54
            if is_stp_ldp:
                rt = insn & 0x1F
                rt2 = (insn >> 10) & 0x1F
                if rt in (19, 20) or rt2 in (19, 20):
                    issues.append(f"  {s.name} @ offset {i}: STP/LDP with x{rt}/x{rt2}")

    if issues:
        print("ERROR: SimStack stencils have compiler-generated x19/x20 save/restore!", file=sys.stderr)
        print("The compiler used callee-saved registers, which defeats register caching.", file=sys.stderr)
        for issue in issues:
            print(issue, file=sys.stderr)
        print("\nThis usually means a stencil is too complex for the compiler to", file=sys.stderr)
        print("fit in caller-saved registers (x0-x18). Simplify the stencil.", file=sys.stderr)
        sys.exit(1)
    else:
        print(f"  SimStack register safety: OK ({len(SIMSTACK_STENCILS)} stencils verified)")


def detect_arch(data: bytes) -> str:
    """Detect the architecture from the Mach-O header."""
    cputype = struct.unpack_from("<I", data, 4)[0]
    if cputype == CPU_TYPE_ARM64:
        return "arm64"
    elif cputype == CPU_TYPE_X86_64:
        return "x86_64"
    else:
        raise ValueError(f"Unknown CPU type: 0x{cputype:08X}")


def extract_stencils(obj_path: Path, arch: str = None) -> List[StencilInfo]:
    """Parse the object file and extract stencil info."""
    data = obj_path.read_bytes()
    if arch is None:
        arch = detect_arch(data)
    parser = MachOParser(data)
    text_code = parser.get_text_code()
    relocs = parser.get_relocations()

    # Find stencil functions (symbols starting with _stencil_)
    stencil_syms = []
    for sym in parser.symbols:
        if sym["name"].startswith("_stencil_") and (sym["type"] & 0x0E) == N_SECT:
            stencil_syms.append(sym)

    # Sort by address
    stencil_syms.sort(key=lambda s: s["value"])

    # Calculate sizes (distance between consecutive symbols)
    text_sec = parser.text_section
    text_start = text_sec["addr"]
    text_end = text_start + text_sec["size"]

    stencils = []
    for i, sym in enumerate(stencil_syms):
        start = sym["value"]
        if i + 1 < len(stencil_syms):
            end = stencil_syms[i + 1]["value"]
        else:
            end = text_end

        name = sym["name"].lstrip("_")  # Remove Mach-O leading underscore
        file_start = start - text_start
        size = end - start
        code = text_code[file_start:file_start + size]

        # Filter relocations for this stencil
        stencil_relocs = []
        for r in relocs:
            if r["offset"] >= file_start and r["offset"] < file_start + size:
                if r["symbol"] and r["symbol"] in HOLE_KIND_MAP:
                    stencil_relocs.append(Relocation(
                        offset=r["offset"] - file_start,
                        reloc_type=r["type"],
                        symbol=r["symbol"],
                        addend=r["addend"],
                    ))

        stencils.append(StencilInfo(
            name=name,
            offset=file_start,
            size=size,
            code=code,
            relocs=stencil_relocs,
        ))

    return stencils


# ===== CODE GENERATION =====

def format_code_bytes(code: bytes, indent: str = "    ") -> str:
    """Format bytes as a C array initializer."""
    lines = []
    for i in range(0, len(code), 16):
        chunk = code[i:i+16]
        hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"{indent}{hex_vals},")
    return "\n".join(lines)


def generate_header(stencils: List[StencilInfo], arch: str) -> str:
    """Generate the C++ header file for a specific architecture."""
    reloc_type_map = ARM64_RELOC_TYPE_MAP if arch == "arm64" else X86_64_RELOC_TYPE_MAP
    arch_upper = arch.upper().replace("-", "_")
    arch_guard = f"__aarch64__" if arch == "arm64" else "__x86_64__"

    lines = []
    lines.append(f"""\
/*
 * generated_stencils_{arch}.hpp - Auto-generated {arch_upper} stencil machine code
 *
 * Generated by scripts/extract_stencils.py — DO NOT EDIT
 *
 * Contains {arch_upper} machine code stencils extracted from compiled C++ bytecode
 * handlers. Each stencil is a byte array + relocation table used by the
 * copy-and-patch JIT compiler.
 */

#ifndef PHARO_GENERATED_STENCILS_{arch_upper}_HPP
#define PHARO_GENERATED_STENCILS_{arch_upper}_HPP

#include "Stencil.hpp"

#if PHARO_JIT_ENABLED && defined({arch_guard})

namespace pharo {{
namespace jit {{
namespace generated {{
""")

    # Emit each stencil's code and relocation data
    for s in stencils:
        lines.append(f"// ----- {s.name} ({s.size} bytes, {len(s.relocs)} relocs) -----")
        lines.append(f"static const uint8_t {s.name}_code[] = {{")
        lines.append(format_code_bytes(s.code))
        lines.append("};")
        lines.append("")

        if s.relocs:
            lines.append(f"static const Relocation {s.name}_relocs[] = {{")
            for r in s.relocs:
                rtype = reloc_type_map.get(r.reloc_type)
                if rtype is None:
                    print(f"  WARNING: Unknown reloc type {r.reloc_type} in {s.name} at offset {r.offset}")
                    rtype = f"static_cast<RelocType>({r.reloc_type})"

                hkind = HOLE_KIND_MAP.get(r.symbol, "HoleKind::Continue")

                # For runtime helpers, encode the helper ID in the addend
                addend = r.addend
                if hkind == "HoleKind::RuntimeHelper":
                    addend = RUNTIME_HELPER_ID.get(r.symbol, 0)

                lines.append(f"    {{ {r.offset}, {rtype}, {hkind}, {addend} }},")
            lines.append("};")
        else:
            lines.append(f"static const Relocation* {s.name}_relocs = nullptr;")
        lines.append("")

    # Emit the stencil table
    lines.append("// ===== STENCIL TABLE =====")
    lines.append("")
    lines.append(f"static constexpr int NumStencils = {len(stencils)};")
    lines.append("")

    # Enum for stencil indices
    lines.append("enum class StencilID : uint16_t {")
    for i, s in enumerate(stencils):
        lines.append(f"    {s.name} = {i},")
    lines.append("};")
    lines.append("")

    # Stencil definition table
    lines.append("static const StencilDef stencilTable[] = {")
    for s in stencils:
        nrelocs = len(s.relocs)
        relocs_ptr = f"{s.name}_relocs" if nrelocs > 0 else "nullptr"
        lines.append(
            f'    {{ "{s.name}", {s.name}_code, {s.size}, '
            f'{relocs_ptr}, {nrelocs}, 1 }},')
    lines.append("};")
    lines.append("")

    # Helper to look up by name
    lines.append("""// Look up a stencil by name (for debugging)
static inline const StencilDef* findStencil(const char* name) {
    for (int i = 0; i < NumStencils; i++) {
        if (__builtin_strcmp(stencilTable[i].name, name) == 0)
            return &stencilTable[i];
    }
    return nullptr;
}""")

    lines.append("")
    lines.append("} // namespace generated")
    lines.append("} // namespace jit")
    lines.append("} // namespace pharo")
    lines.append("")
    lines.append(f"#endif // PHARO_JIT_ENABLED && {arch_guard}")
    lines.append(f"#endif // PHARO_GENERATED_STENCILS_{arch_upper}_HPP")
    lines.append("")

    return "\n".join(lines)


def generate_dispatcher_header(architectures: List[str]) -> str:
    """Generate a dispatcher header that includes the right arch-specific header."""
    lines = []
    lines.append("""\
/*
 * generated_stencils.hpp - Architecture dispatcher for generated stencils
 *
 * Generated by scripts/extract_stencils.py — DO NOT EDIT
 *
 * Includes the correct architecture-specific stencil header based on
 * the target platform.
 */

#ifndef PHARO_GENERATED_STENCILS_HPP
#define PHARO_GENERATED_STENCILS_HPP
""")
    for arch in architectures:
        guard = "__aarch64__" if arch == "arm64" else "__x86_64__"
        lines.append(f"#if defined({guard})")
        lines.append(f'  #include "generated_stencils_{arch}.hpp"')
        lines.append(f"#endif")
        lines.append("")

    lines.append("#endif // PHARO_GENERATED_STENCILS_HPP")
    lines.append("")
    return "\n".join(lines)


# ===== MAIN =====

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Extract JIT stencils from compiled object files")
    parser.add_argument("--arch", choices=["arm64", "x86_64", "all"], default="all",
                        help="Target architecture (default: all)")
    args = parser.parse_args()

    architectures = ["arm64", "x86_64"] if args.arch == "all" else [args.arch]

    print("=== Stencil Extraction ===")
    print(f"  Architectures: {', '.join(architectures)}")
    print()

    output_dir = OUTPUT_HPP.parent

    for arch in architectures:
        print(f"--- {arch} ---")

        # Step 1: Compile
        steps = 4 if arch == "arm64" else 3
        print(f"[1/{steps}] Compiling stencils ({arch})...")
        obj_path = compile_stencils(arch)
        print()

        # Step 2: Extract
        print(f"[2/{steps}] Extracting stencils from Mach-O ({arch})...")
        stencils = extract_stencils(obj_path, arch)
        print(f"  Found {len(stencils)} stencils:")
        total_code = 0
        total_relocs = 0
        for s in stencils:
            print(f"    {s.name:30s}  {s.size:4d} bytes  {len(s.relocs):2d} relocs")
            total_code += s.size
            total_relocs += len(s.relocs)
        print(f"  Total: {total_code} bytes code, {total_relocs} relocations")
        print()

        # Step 3: Verify SimStack register safety (ARM64 only)
        if arch == "arm64":
            print(f"[3/{steps}] Verifying SimStack register safety...")
            verify_simstack_register_safety(stencils, arch)
            print()

        # Generate arch-specific header
        print(f"[{steps}/{steps}] Generating header ({arch})...")
        header = generate_header(stencils, arch)
        arch_hpp = output_dir / f"generated_stencils_{arch}.hpp"
        arch_hpp.write_text(header)
        print(f"  Written: {arch_hpp} ({len(header)} bytes)")
        print()

    # Generate dispatcher header
    print("Generating dispatcher header...")
    dispatcher = generate_dispatcher_header(architectures)
    OUTPUT_HPP.write_text(dispatcher)
    print(f"  Written: {OUTPUT_HPP} ({len(dispatcher)} bytes)")
    print()
    print("Done!")


if __name__ == "__main__":
    main()
