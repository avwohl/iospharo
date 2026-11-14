# pharo-cig: `CXString` is declared as one pointer, libclang uses a 16-byte struct

This text is a draft of an issue for the pharo-cig maintainers.  It is not
filed.  The fix is `docs/patches/pharo-cig-cxstring.st`.

## Defect

libclang declares:

    typedef struct { const void *data; unsigned private_flags; } CXString;

libclang returns a `CXString` by value and takes a `CXString` by value in
`clang_getCString` and `clang_disposeString`.  CIG (commit `e714f8fd`)
declares `CXString` as an `FFIExternalObject`, which is one pointer.

- On a return, the VM keeps `data` and drops `private_flags`.
- On `clang_disposeString(self)`, the VM passes `data` only.  The second
  argument register (x1 on arm64, rsi on x86_64) holds stale data.
- `clang_disposeString` calls `free(data)` when `private_flags` is 1.  When
  the stale register is 1 and the string is not from `malloc`, the process
  stops with "pointer being freed was not allocated".
- When the stale register is not 1, a string from `malloc` is not freed.

The defect is in the declaration, so each VM has it.  How frequently the
stale register is 1 depends on the VM and on the machine.

## Fix

Declare `CXString` as a `CXStructure` with the fields `void *data` and
`uint private_flags`, and pass `CXString self` by value to
`clang_getCString` and `clang_disposeString`.  `CXTranslationUnit>>tokenSpelling:`
must use `string isNull`, not `getHandle isNull`.

## Measured

- Unpatched, full `CIG-Tests`, clean C++ VM on arm64: 10 of 12 runs stopped
  with SIGABRT.  In 8 of 8 aborts with a call logger, the last
  `clang_disposeString` call had 1 in x1 and a `data` pointer that is not a
  `malloc` block.
- Patched: 12 of 12 runs completed, pass=318 fail=18 err=6.  In 40478
  `clang_disposeString` calls, x1 was 0 or 1 only, and each call with 1 had
  a `malloc` block.
- Stock Pharo VM (x86_64): the unpatched suite completes with the same
  318/18/6.  The stale register was 1 in one of 21053 calls, and that string
  was from `malloc`.
