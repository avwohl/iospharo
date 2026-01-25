# Debugging Loops - Patterns I've Gotten Stuck On

This document tracks patterns where I've looped or made incorrect conclusions,
to prevent repeating the same mistakes.

## 1. FFI "Incomplete" Loop (2026-01-25)

**Pattern:** Concluded FFI was incomplete, then repeatedly checked and found it exists.

**What happened:**
- I saw that OSSDL2Driver wasn't starting its event loop
- Concluded "FFI isn't working" without verifying
- Kept checking if FFI primitives existed (they do)
- Went in circles saying "FFI incomplete" then "FFI exists"

**Root cause:**
Named primitive lookup for `isVMDisplayUsingSDL2` was failing because:
- Module name extraction was getting wrong value (class name instead of module)
- The key format didn't match what was registered

**Fix:** Added direct string literal check in primitiveExternalCall for known primitives.

**Lesson:** When FFI "doesn't work", check:
1. Is the primitive being called at all? (check logs)
2. Is the lookup finding the right handler? (check key matching)
3. Is the handler returning the right value? (check return path)

## 2. Symbol Lookup Using Wrong Class (2026-01-25)

**Pattern:** Symbol lookup returned nil even though symbols existed.

**What happened:**
- Used `Symbol` class (classIdx=3095) to find symbols
- But actual symbols are `ByteSymbol` instances (classIdx=3085)

**Fix:** Look up ByteSymbol class and use its classIndex for matching.

**Lesson:** Pharo has multiple symbol types. ByteSymbol is the concrete class.

## 3. Memory Scan Wrong Boundary (2026-01-25)

**Pattern:** Memory scans found nothing, even in large heaps.

**What happened:**
- Scanned to `oldSpaceEnd_` (allocated buffer size ~536MB)
- Should scan to `oldSpaceFree_` (actual data ~268MB)
- Was reading uninitialized memory

**Fix:** Use `oldSpaceFree_` as the scan boundary.

**Lesson:** Always use the "free pointer" not the "end pointer" for scans.

## General Anti-Patterns

1. **Concluding X doesn't work without checking logs** - Always check the relevant log first
2. **Assuming implementation matches expectation** - Verify the actual code path
3. **Not verifying data format** - Check what format the method literals are actually using
4. **Repeating the same search** - If you've searched 3 times, the problem is elsewhere
