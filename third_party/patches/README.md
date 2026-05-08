# Third-party patches

Local patches applied on top of submodules at configure time.
`CMakeLists.txt` walks `asmjit-*.patch` (sorted) and applies each one
that isn't already applied — idempotent, so re-running cmake is safe.

If you bump a submodule SHA, re-test these patches; remove or refresh
any that no longer apply cleanly.

## asmjit-catalyst-virtmem.patch

`asmjit/core/virtmem.cpp` only includes `<libkern/OSCacheControl.h>`
under `TARGET_OS_OSX`, but `sys_icache_invalidate()` is also called on
Mac Catalyst (where `TARGET_OS_OSX == 0`). Without the include,
Catalyst builds fail with "undeclared identifier".

The patch makes the include unconditional on all Apple platforms.

Upstream: not yet submitted.
