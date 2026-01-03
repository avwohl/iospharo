# iOS VM Debugging Scripts

These scripts set up the VMMaker simulation environment for debugging the iOS memory allocation issue.

## The Problem

iOS crashes at EXC_BAD_ACCESS accessing address `0x100010db558` - an unrelocated pointer. The VM expects to allocate memory at fixed addresses (1TB for oldSpace), but iOS ASLR prevents this. The fix requires understanding exactly when pointers are accessed before relocation completes.

## Quick Start

```bash
# Launch VMMaker with the Pharo VM
./launch-vmmaker.sh
```

## In Pharo Playground

### 1. Load the debugging scripts

```smalltalk
(FileStream fileNamed: '/Users/wohl/src/pharo/iospharo/scripts/ios-simulator-setup.st') fileIn.
(FileStream fileNamed: '/Users/wohl/src/pharo/iospharo/scripts/debug-unrelocated-pointers.st') fileIn.
```

### 2. Create and configure simulator

```smalltalk
| sim |
sim := StackInterpreterSimulator newWithOptions: #().
sim traceUnrelocatedAccess.
sim setBreakSelector: #swizzleObj:.
sim openOn: '/path/to/test.image'.
sim openAsMorph; run.
```

### 3. When breakpoint hits

```smalltalk
sim dumpSegmentInfo.
sim checkPointer: 16r100010db558.
```

## Files

- `launch-vmmaker.sh` - Shell script to launch VMMaker image
- `ios-simulator-setup.st` - Basic iOS simulator configuration
- `debug-unrelocated-pointers.st` - Detailed debugging instrumentation

## Key Classes to Study

- `StackInterpreterSimulator` - Main interpreter simulator
- `Spur64BitMMLESimulator` - 64-bit memory manager simulator
- `SpurSegmentManager>>swizzleObj:` - Pointer relocation function
- `SpurMemoryManager>>adjustAllOopsBy:` - Bulk relocation

## Debugging Strategy

1. Use Lemming Debugging: clone simulator state before crash
2. Trace `swizzleObj:` calls to see relocation sequence
3. Find memory accesses that happen BEFORE swizzle completes
4. Once found, port the fix to C code in `iosMemory.c`
