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
    "-fno-split-machine-functions",
    "-mllvm", "-hot-cold-split=false",
    "-mllvm", "-enable-machine-outliner=never",
    # todo.md §2.5 experiment 2026-04-18: compiling with -Os
    # shrinks stencil_sendJ2J from 2092 → 1752 bytes (-16%, total
    # 11918 → 10631) but regresses bench 10-15% (fast-mode runs
    # drop from ~205ms to ~229ms, slow-mode from ~375ms to ~388ms).
    # Not worth the tradeoff for a perf-focused build.  Revert.
    # iOS-focused builds (where code-zone size matters more) could
    # flip this knob.
]

# Host-OS-aware target triple selection.  Mac side (used when this script
# runs on macOS) uses Apple-darwin targets so the output is Mach-O.  Linux
# side uses linux-gnu so clang emits ELF objects.  Both feed the same
# extractor because the parser dispatches on the file's magic bytes.
import platform
_IS_DARWIN = platform.system() == "Darwin"
_IS_LINUX = platform.system() == "Linux"
if _IS_DARWIN:
    CFLAGS_ARM64 = CFLAGS_COMMON + ["-target", "arm64-apple-macos14"]
    CFLAGS_X86_64 = CFLAGS_COMMON + ["-target", "x86_64-apple-macos14"]
else:
    # Linux (or anything non-Darwin): emit ELF for the requested arch.
    CFLAGS_ARM64 = CFLAGS_COMMON + ["-target", "aarch64-linux-gnu"]
    CFLAGS_X86_64 = CFLAGS_COMMON + ["-target", "x86_64-linux-gnu"]

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
    "__HOLE_RT_NEW_PRIM": "HoleKind::RuntimeHelper",
    "__HOLE_RT_IC_MISS": "HoleKind::RuntimeHelper",
    "__HOLE_RT_J2J_TRACE": "HoleKind::RuntimeHelper",
    "__HOLE_RT_PRIMAT_PTR": "HoleKind::RuntimeHelper",
    "__HOLE_RT_PRIMATPUT_PTR": "HoleKind::RuntimeHelper",
    "__HOLE_RT_RECOMPILE_QUEUE": "HoleKind::RuntimeHelper",
    "__HOLE_RT_FILL_IC": "HoleKind::RuntimeHelper",
    "__HOLE_RT_WRITE_BARRIER": "HoleKind::RuntimeHelper",
    "__HOLE_RT_PRIMSIZE_PTR": "HoleKind::RuntimeHelper",
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
    "__HOLE_RT_NEW_PRIM": 12,
    "__HOLE_RT_IC_MISS": 13,
    "__HOLE_RT_J2J_TRACE": 14,
    "__HOLE_RT_PRIMAT_PTR": 15,
    "__HOLE_RT_PRIMATPUT_PTR": 16,
    "__HOLE_RT_RECOMPILE_QUEUE": 17,
    "__HOLE_RT_FILL_IC": 18,
    "__HOLE_RT_WRITE_BARRIER": 19,
    "__HOLE_RT_PRIMSIZE_PTR": 20,
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


# ===== ELF CONSTANTS =====
# ELF64 little-endian — sufficient for x86_64-linux-gnu and
# aarch64-linux-gnu object files emitted by clang.

ELF_MAGIC = b"\x7fELF"

# e_machine values
EM_X86_64 = 62
EM_AARCH64 = 183

# Section header types
SHT_NULL     = 0
SHT_PROGBITS = 1
SHT_SYMTAB   = 2
SHT_STRTAB   = 3
SHT_RELA     = 4
SHT_NOBITS   = 8

# Symbol info: low nibble of st_info = type, high nibble = bind
STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC   = 2
STT_SECTION = 3

# x86_64 ELF relocation types (relevant subset, from elf.h)
R_X86_64_NONE          = 0
R_X86_64_64            = 1
R_X86_64_PC32          = 2
R_X86_64_PLT32         = 4
R_X86_64_GOTPCREL      = 9
R_X86_64_GOTPCRELX     = 41
R_X86_64_REX_GOTPCRELX = 42

# aarch64 ELF relocation types (relevant subset)
R_AARCH64_CALL26          = 283
R_AARCH64_JUMP26          = 282
R_AARCH64_ADR_PREL_PG_HI21 = 275
R_AARCH64_ADD_ABS_LO12_NC  = 277
R_AARCH64_LDST64_ABS_LO12_NC = 286
R_AARCH64_ADR_GOT_PAGE     = 311
R_AARCH64_LD64_GOT_LO12_NC = 312

# Mapping ELF reloc type → the Mach-O equivalent we already pass to the
# generated header.  The runtime header treats x86_64 BRANCH/GOT_LOAD/GOT
# all as RelocType::X86_64_PC32 (a single rel32 patch site), so for the
# Linux build any rel32-style reloc maps to the same X86_64_RELOC_BRANCH
# constant used on the Mac path.  For aarch64, ELF and Mach-O have
# separate but similar enums; map them so the same downstream logic
# (HoleKind selection, addend handling) keeps working.
ELF_X86_64_TO_MACHO = {
    R_X86_64_PC32:          X86_64_RELOC_BRANCH,
    R_X86_64_PLT32:         X86_64_RELOC_BRANCH,
    R_X86_64_GOTPCREL:      X86_64_RELOC_GOT_LOAD,
    R_X86_64_GOTPCRELX:     X86_64_RELOC_GOT_LOAD,
    R_X86_64_REX_GOTPCRELX: X86_64_RELOC_GOT_LOAD,
}
ELF_AARCH64_TO_MACHO = {
    R_AARCH64_CALL26:              ARM64_RELOC_BRANCH26,
    R_AARCH64_JUMP26:              ARM64_RELOC_BRANCH26,
    R_AARCH64_ADR_PREL_PG_HI21:    ARM64_RELOC_PAGE21,
    R_AARCH64_ADD_ABS_LO12_NC:     ARM64_RELOC_PAGEOFF12,
    R_AARCH64_ADR_GOT_PAGE:        ARM64_RELOC_GOT_LOAD_PAGE21,
    R_AARCH64_LD64_GOT_LO12_NC:    ARM64_RELOC_GOT_LOAD_PAGEOFF12,
}


class ELFParser:
    """Minimal ELF64 little-endian parser, modeled to expose the same shape
    of information the rest of this script reads from MachOParser:

      - parser.symbols           list of {name,type,sect,desc,value}
      - parser.sections          list of section dicts
      - parser.text_section      the .text section dict (with key "size"
                                 + "fileoff" — same names MachOParser uses,
                                 mapped from sh_size / sh_offset)
      - parser.get_text_code()   raw bytes of .text
      - parser.get_relocations() list of {offset,type,pcrel,extern,length,
                                         symbol,addend} where `type` is the
                                 Mach-O equivalent (so the existing reloc
                                 type maps just work).
    """

    def __init__(self, data: bytes):
        self.data = data
        self.symbols: List[dict] = []
        self.sections: List[dict] = []
        self.section_names: List[str] = []  # by index
        self.text_section: Optional[dict] = None
        self._text_idx = -1
        self._symtab_idx = -1
        self._strtab_idx = -1
        self._reloc_sections: List[dict] = []  # SHT_RELA targeting .text
        self._machine = 0
        self._parse()

    def _parse(self):
        if self.data[:4] != ELF_MAGIC:
            raise ValueError("Not an ELF file")
        ei_class = self.data[4]
        ei_data = self.data[5]
        if ei_class != 2 or ei_data != 1:
            raise ValueError(
                f"Only ELF64 little-endian supported (class={ei_class}, data={ei_data})")

        # Elf64_Ehdr after e_ident (16 bytes):
        # H e_type, H e_machine, I e_version, Q e_entry, Q e_phoff,
        # Q e_shoff, I e_flags, H e_ehsize, H e_phentsize, H e_phnum,
        # H e_shentsize, H e_shnum, H e_shstrndx
        e_type, e_machine, e_version, e_entry, e_phoff, \
        e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum, \
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(
            "<HHIQQQIHHHHHH", self.data, 16)
        self._machine = e_machine

        # Section header string table — names of sections live in this
        # SHT_STRTAB.  Read it first so we can name sections as we parse.
        shstr_off, shstr_size = struct.unpack_from(
            "<QQ", self.data, e_shoff + e_shstrndx * e_shentsize + 24)
        shstrtab = self.data[shstr_off:shstr_off + shstr_size]

        # Pass 1: collect sections.
        for i in range(e_shnum):
            sh = e_shoff + i * e_shentsize
            sh_name, sh_type, sh_flags, sh_addr, sh_offset, \
            sh_size, sh_link, sh_info, sh_addralign, sh_entsize = \
                struct.unpack_from("<IIQQQQIIQQ", self.data, sh)
            name_end = shstrtab.index(b"\0", sh_name)
            name = shstrtab[sh_name:name_end].decode("ascii")
            sec = {
                "index":    i,
                "name":     name,
                "type":     sh_type,
                "flags":    sh_flags,
                "addr":     sh_addr,
                # MachOParser-compatible aliases:
                "fileoff":  sh_offset,
                "size":     sh_size,
                "sectname": name,
                "segname":  "",  # ELF has no segname; leave blank
                "reloff":   0,   # filled when we walk relocs
                "nreloc":   0,
                # ELF-specific:
                "sh_link":  sh_link,
                "sh_info":  sh_info,
                "sh_entsize": sh_entsize,
            }
            self.sections.append(sec)
            self.section_names.append(name)
            if sh_type == SHT_SYMTAB:
                self._symtab_idx = i
                self._strtab_idx = sh_link
            if name == ".text" and sh_type == SHT_PROGBITS:
                self._text_idx = i
                self.text_section = sec

        # Pass 2: collect relocation sections targeting .text.
        for sec in self.sections:
            if sec["type"] == SHT_RELA and sec["sh_info"] == self._text_idx:
                self._reloc_sections.append(sec)

        # Parse symbol table.
        if self._symtab_idx >= 0:
            symtab = self.sections[self._symtab_idx]
            strtab = self.sections[self._strtab_idx]
            strdata = self.data[strtab["fileoff"]:
                                strtab["fileoff"] + strtab["size"]]
            ent = symtab["sh_entsize"] or 24  # Elf64_Sym is 24 bytes
            n = symtab["size"] // ent
            for i in range(n):
                so = symtab["fileoff"] + i * ent
                st_name, st_info, st_other, st_shndx, st_value, st_size = \
                    struct.unpack_from("<IBBHQQ", self.data, so)
                ne = strdata.index(b"\0", st_name)
                name = strdata[st_name:ne].decode("ascii")
                # Normalize symbol names to the Mach-O convention the
                # rest of the script expects.  Mach-O prepends `_` to
                # every C symbol, so `stencil_pushTemp` becomes
                # `_stencil_pushTemp` and `_HOLE_CONTINUE` becomes
                # `__HOLE_CONTINUE`.  ELF leaves the C name unchanged,
                # so always prepend `_` here for parity.
                if name:
                    name = "_" + name
                # Translate to MachOParser's symbol fields so the rest of
                # the script can ignore the difference.  N_SECT for Mach-O
                # was 0x0E; we mirror that for symbols defined in any
                # ELF section (st_shndx != SHN_UNDEF=0).
                type_byte = N_SECT if st_shndx != 0 else 0
                self.symbols.append({
                    "name":  name,
                    "type":  type_byte,
                    "sect":  st_shndx,
                    "desc":  st_info,  # repurposed: ELF st_info
                    "value": st_value,
                    "_elf_st_size": st_size,
                })

    def get_text_code(self) -> bytes:
        sec = self.text_section
        if not sec:
            raise ValueError("No .text section found")
        return self.data[sec["fileoff"]:sec["fileoff"] + sec["size"]]

    def get_relocations(self) -> List[dict]:
        """Return relocations for .text in MachOParser-compatible shape."""
        result = []
        for relsec in self._reloc_sections:
            ent = relsec["sh_entsize"] or 24  # Elf64_Rela is 24 bytes
            n = relsec["size"] // ent
            for i in range(n):
                ro = relsec["fileoff"] + i * ent
                r_offset, r_info, r_addend = struct.unpack_from(
                    "<QQq", self.data, ro)  # addend is signed
                r_sym = (r_info >> 32) & 0xFFFFFFFF
                r_type_elf = r_info & 0xFFFFFFFF

                # Symbol name from symbol table.
                sym_name = ""
                if r_sym < len(self.symbols):
                    sym_name = self.symbols[r_sym]["name"]

                # Map ELF reloc type → Mach-O code so downstream code
                # (HoleKind/RelocType selection) just works.
                if self._machine == EM_X86_64:
                    macho_type = ELF_X86_64_TO_MACHO.get(r_type_elf)
                elif self._machine == EM_AARCH64:
                    macho_type = ELF_AARCH64_TO_MACHO.get(r_type_elf)
                else:
                    macho_type = None
                if macho_type is None:
                    # Skip relocs we don't know how to translate.  Could
                    # be SHT_RELA-style absolute relocs we don't need.
                    continue

                result.append({
                    "offset":  r_offset,
                    "type":    macho_type,
                    "pcrel":   True,   # all rel32-class relocs are pcrel
                    "extern":  True,
                    "length":  2,      # 32-bit displacement
                    "symbol":  sym_name,
                    "addend":  r_addend,
                })
        return result


def detect_object_format(data: bytes) -> str:
    """Return 'macho' or 'elf' based on the file's magic."""
    if len(data) >= 4:
        magic32 = struct.unpack_from("<I", data, 0)[0]
        if magic32 in (MH_MAGIC_64, MH_CIGAM_64):
            return "macho"
        if data[:4] == ELF_MAGIC:
            return "elf"
    raise ValueError(f"Unknown object-file format (first 4 bytes: {data[:4]!r})")


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


# SimStack stencils that use x19-x22 via inline asm (ARM64 only).
# These rely on the compiler NOT using x19-x22 for its own purposes.
# After compilation, we verify this by checking for STP/LDP save/restore.
SIMSTACK_STENCILS = {
    "stencil_flush1", "stencil_flush2", "stencil_flush3", "stencil_flush4",
    "stencil_pushTemp_E", "stencil_pushTemp_1", "stencil_pushTemp_2",
    "stencil_pushTemp_3", "stencil_pushTemp_4",
    "stencil_pushRecvVar_E", "stencil_pushRecvVar_1", "stencil_pushRecvVar_2",
    "stencil_pushRecvVar_3", "stencil_pushRecvVar_4",
    "stencil_pushLitConst_E", "stencil_pushLitConst_1", "stencil_pushLitConst_2",
    "stencil_pushLitConst_3", "stencil_pushLitConst_4",
    "stencil_pushLitVar_E", "stencil_pushLitVar_1", "stencil_pushLitVar_2",
    "stencil_pushLitVar_3", "stencil_pushLitVar_4",
    "stencil_pushReceiver_E", "stencil_pushReceiver_1", "stencil_pushReceiver_2",
    "stencil_pushReceiver_3", "stencil_pushReceiver_4",
    "stencil_pushTrue_E", "stencil_pushTrue_1", "stencil_pushTrue_2",
    "stencil_pushTrue_3", "stencil_pushTrue_4",
    "stencil_pushFalse_E", "stencil_pushFalse_1", "stencil_pushFalse_2",
    "stencil_pushFalse_3", "stencil_pushFalse_4",
    "stencil_pushNil_E", "stencil_pushNil_1", "stencil_pushNil_2",
    "stencil_pushNil_3", "stencil_pushNil_4",
    "stencil_pop_2", "stencil_pop_1", "stencil_pop_E",
    "stencil_pop_3", "stencil_pop_4",
    "stencil_dup_E", "stencil_dup_1", "stencil_dup_2",
    "stencil_dup_3", "stencil_dup_4",
    "stencil_storeTemp_1", "stencil_storeTemp_2",
    "stencil_popStoreTemp_2", "stencil_popStoreTemp_1",
    "stencil_popStoreTemp_3", "stencil_popStoreTemp_4",
    "stencil_storeRecvVar_1",
    "stencil_popStoreRecvVar_2", "stencil_popStoreRecvVar_1",
    "stencil_popStoreRecvVar_3", "stencil_popStoreRecvVar_4",
    "stencil_addSmallInt_2", "stencil_subSmallInt_2", "stencil_mulSmallInt_2",
    "stencil_addSmallInt_3", "stencil_subSmallInt_3", "stencil_mulSmallInt_3",
    "stencil_addSmallInt_4", "stencil_subSmallInt_4", "stencil_mulSmallInt_4",
    "stencil_lessThanSmallInt_2", "stencil_greaterThanSmallInt_2",
    "stencil_lessEqualSmallInt_2", "stencil_greaterEqualSmallInt_2",
    "stencil_equalSmallInt_2", "stencil_notEqualSmallInt_2",
    "stencil_lessThanSmallInt_3", "stencil_greaterThanSmallInt_3",
    "stencil_lessEqualSmallInt_3", "stencil_greaterEqualSmallInt_3",
    "stencil_equalSmallInt_3", "stencil_notEqualSmallInt_3",
    "stencil_lessThanSmallInt_4", "stencil_greaterThanSmallInt_4",
    "stencil_lessEqualSmallInt_4", "stencil_greaterEqualSmallInt_4",
    "stencil_equalSmallInt_4", "stencil_notEqualSmallInt_4",
    "stencil_jumpTrue_1", "stencil_jumpFalse_1",
    "stencil_jumpTrueBack_1", "stencil_jumpFalseBack_1",
    # stencil_returnTop_E, stencil_returnTop_1, stencil_returnReceiver_1
    # all intentionally omitted: these are terminal return stencils — after
    # them no further stencil runs in the activation, so x19-x22 SimStack
    # values no longer need to be preserved.  This allows the B5 tracing
    # helpers (events 2/3/4/5) to be added to their return paths without
    # the verifier flagging the resulting compiler spills.
}


def verify_simstack_register_safety(stencils: List[StencilInfo], arch: str):
    """Verify that SimStack stencils don't have compiler-generated x19-x22 save/restore.

    The register-based SimStack approach relies on the compiler NOT touching
    x19-x22 in these small tail-call functions. If the compiler generates
    STP/LDP for x19-x22, our inline asm writes would be undone by the restore.
    """
    if arch != "arm64":
        return  # Only relevant for ARM64

    # ARM64 STP/LDP encoding for x19-x22:
    # We check for any STP/LDP involving x19-x22 (registers 19-22)
    # in the Rt/Rt2 fields.
    simstack_regs = {19, 20, 21, 22}
    issues = []
    for s in stencils:
        if s.name not in SIMSTACK_STENCILS:
            continue
        code = s.code
        for i in range(0, len(code) - 3, 4):
            insn = struct.unpack_from("<I", code, i)[0]
            # STP/LDP GP 64-bit: bits 31:25 = 1010100 = 0x54
            is_stp_ldp = ((insn >> 25) & 0x7F) == 0x54
            if is_stp_ldp:
                rt = insn & 0x1F
                rt2 = (insn >> 10) & 0x1F
                if rt in simstack_regs or rt2 in simstack_regs:
                    issues.append(f"  {s.name} @ offset {i}: STP/LDP with x{rt}/x{rt2}")

    if issues:
        print("ERROR: SimStack stencils have compiler-generated x19-x22 save/restore!", file=sys.stderr)
        print("The compiler used callee-saved registers, which defeats register caching.", file=sys.stderr)
        for issue in issues:
            print(issue, file=sys.stderr)
        print("\nThis usually means a stencil is too complex for the compiler to", file=sys.stderr)
        print("fit in caller-saved registers (x0-x18). Simplify the stencil.", file=sys.stderr)
        sys.exit(1)
    else:
        print(f"  SimStack register safety: OK ({len(SIMSTACK_STENCILS)} stencils verified)")


def detect_arch(data: bytes) -> str:
    """Detect the architecture from a Mach-O or ELF header."""
    fmt = detect_object_format(data)
    if fmt == "macho":
        cputype = struct.unpack_from("<I", data, 4)[0]
        if cputype == CPU_TYPE_ARM64:
            return "arm64"
        elif cputype == CPU_TYPE_X86_64:
            return "x86_64"
        raise ValueError(f"Unknown Mach-O CPU type: 0x{cputype:08X}")
    else:  # ELF
        e_machine = struct.unpack_from("<H", data, 18)[0]
        if e_machine == EM_AARCH64:
            return "arm64"
        if e_machine == EM_X86_64:
            return "x86_64"
        raise ValueError(f"Unknown ELF e_machine: {e_machine}")


def extract_stencils(obj_path: Path, arch: str = None) -> List[StencilInfo]:
    """Parse the object file and extract stencil info."""
    data = obj_path.read_bytes()
    fmt = detect_object_format(data)
    if arch is None:
        arch = detect_arch(data)
    if fmt == "macho":
        parser = MachOParser(data)
    else:
        parser = ELFParser(data)
    text_code = parser.get_text_code()
    relocs = parser.get_relocations()

    # Find stencil functions.  Both Mach-O and ELF symbol streams now
    # carry a leading `_` (ELF parser re-adds it) so a single prefix
    # matches both formats.  Skip .cold.N fragments — compiler-split
    # cold paths that must remain part of their parent stencil (they're
    # reached via branches already inside the parent's code range).
    stencil_syms = []
    for sym in parser.symbols:
        if sym["name"].startswith("_stencil_") and (sym["type"] & 0x0E) == N_SECT:
            if ".cold." not in sym["name"]:
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
    """Generate a dispatcher header that includes the right arch-specific header.

    The dispatcher always references both arm64 and x86_64 — the inner
    #if guards select at build time.  This way, regenerating just one
    arch (e.g. --arch x86_64 on a Linux host) doesn't strip the other
    arch's include from the dispatcher.
    """
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
    for arch in ("arm64", "x86_64"):
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
        fmt_label = "Mach-O" if _IS_DARWIN else "ELF"
        print(f"[2/{steps}] Extracting stencils from {fmt_label} ({arch})...")
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
