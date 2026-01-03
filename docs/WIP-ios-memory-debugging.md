# iOS VM Memory Debugging - Work In Progress

## Problem

The iOS port crashes with EXC_BAD_ACCESS at address `0x100010db558` - an unrelocated pointer.

**Root cause:** The Pharo VM expects to allocate memory at fixed addresses:
- Old space at `0x10000000000` (1TB)
- Perm space at `0x20000000000` (2TB)

iOS ASLR prevents allocating at these addresses, so memory is allocated at lower addresses (e.g., 4GB range). The VM must relocate all pointers in the loaded image by adding `bytesToShift` (the difference between saved and actual addresses). The crash occurs when code accesses a pointer BEFORE relocation completes.

## Current Approach

Using the **StackInterpreterSimulator** to debug in pure Smalltalk:
- Deterministic execution (no ASLR randomization)
- Can set breakpoints on any method
- "Lemming Debugging" - clone state before crash, debug in copy
- Once fixed in Smalltalk, port to C

## Files Created

### iOS Memory Allocation
- `src/ios/iosMemory.c` - Arena-based memory allocation (2GB arena with fixed offsets)

### Debugging Scripts
- `scripts/launch-vmmaker.sh` - Launch VMMaker image
- `scripts/ios-simulator-setup.st` - Basic iOS simulator configuration
- `scripts/debug-unrelocated-pointers.st` - Detailed pointer tracing instrumentation
- `scripts/README.md` - Usage documentation

## Memory Layout (iosMemory.c)

```
Arena (2GB total):
  Offset 0x00000000: Code zone (non-executable on iOS)
  Offset 0x10000000: Stack pages (256MB)
  Offset 0x20000000: New space (512MB)
  Offset 0x40000000: Old space (1GB)
  Offset 0x60000000: Perm space (remaining)
```

## Debugging Session

### Launch VMMaker
```bash
cd /Users/wohl/src/pharo/iospharo/scripts
./launch-vmmaker.sh
```

### In Pharo Playground
```smalltalk
| sim testImage |
testImage := '/path/to/test.image'.
sim := StackInterpreterSimulator newWithOptions: #().
sim setBreakSelector: #adjustAllOopsBy:.
sim openOn: testImage.
sim openAsMorph; run.
```

### Key Methods to Trace
- `SpurMemoryManager>>adjustAllOopsBy:` - Bulk pointer relocation
- `SpurSegmentManager>>swizzleObj:` - Single pointer relocation
- `countNumClassPagesPreSwizzle:` - Pre-swizzle setup

## Next Steps

1. [ ] Run simulator and hit breakpoint at `adjustAllOopsBy:`
2. [ ] Trace which code accesses pointers before swizzle completes
3. [ ] Identify the exact code path causing the crash
4. [ ] Fix in Smalltalk (add guards or reorder initialization)
5. [ ] Port fix to C code in `iosMemory.c` or `cointerp.c`

## References

- `docs/Cross-ISA Testing of the Pharo VM.pdf` - Testing methodology
- `docs/Two Decades of Smalltalk VM Development.pdf` - Simulation infrastructure
- `/Users/wohl/src/pharo/pharo-vm/smalltalksrc/VMMaker/` - VM source in Slang
