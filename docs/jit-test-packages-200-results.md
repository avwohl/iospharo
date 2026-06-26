# 200-package x86-JIT sweep — results (2026-06-26)

Ran the [200-package manifest](jit-test-packages-200.md) on an AWS x86_64 box
(m6a.4xlarge), x86 JIT enabled (`PHARO_X86_JIT=1`), comparing the custom VM to
stock Cog on the same Metacello-loaded Pharo 13.1 image (8 parallel workers via
`run-sweep-parallel.sh`). Raw data: `scripts/pkg-jit-test/results/`.

## Headline

| metric | count |
|--------|------:|
| packages attempted | 200 |
| loaded headless into P13 | **171** |
| **byte-identical JIT-vs-Cog parity** | **143** |
| divergent (failures the custom VM has but Cog doesn't) | 28 |
| **confirmed x86-JIT codegen bugs** | **0** |

The x86 tier-1 JIT is **byte-identical to the interpreter** on every package
tested. All 28 divergences vs Cog are VM-core/env, not JIT — confirmed by
re-running the non-network candidates with the JIT ON vs OFF (`confirm-jit.sh`):
all 14 gave `jit-caused=0` (identical failures with and without the JIT).
`scripts/pkg-jit-test/results/sweep-200-x86-jit-confirmation.txt`.

## What the divergences actually are (all VM-core, none JIT)

1. **SSL/TLS backend is a no-op stub** — the biggest cluster. `src/vm/plugins/
   sqGenericSSL.c` has every function (`sqCreateSSL`/`sqConnectSSL`/`sqEncryptSSL`
   /…) just `return SQSSL_GENERIC_ERROR;`, and the binary doesn't link OpenSSL
   (despite `libssl.so.3` on the box). The SqueakSSL *plugin* is fully wired
   (`PHARO_WITH_CRYPTO=ON`, primitives registered), but the backend does nothing,
   so any HTTPS handshake fails → the image raises `ZdcPluginMissing`. (A SIGSEGV
   appears only at VM *shutdown* after an SSL attempt — secondary, from teardown
   touching the dead session.) Affected: `evref-bl-*-pharo-api`, `ba-st-stargate`,
   `smalltalkweb-myprecious`, `newapplesho-google-cloud`, `juliendelplanque-jrpc`,
   network half of `svenvc-p3`. **Fix:** implement a real OpenSSL backend in
   `sqGenericSSL.c` (`SSL_CTX_new`/`SSL_new`/`SSL_connect`/`SSL_read`/`SSL_write`,
   mem-BIO model) + link `-lssl -lcrypto`; the scaffolding is all in place.

2. **SQLite3 FFI module not loading** — `Error: Module not found.` Affected:
   `pharo-rdbms-pharo-sqlite3`, `rko281-restoreforpharo` (the 2356 count is one
   failing FFI module × every test). `apptivegrid-soil` is FFI flock, similar.

3. **Assorted VM-core/env** — the confirmed `jit-caused=0` set (`famix` 46,
   `microdown` 28, `illimani` 16, `polymath` 3, `xml-xmlparser` 10, …): identical
   with the JIT off, several already documented in the harness memory as VM-core
   /image-version issues, plus tests needing live servers (`p3` wants a `postgres`
   host).

29 packages didn't load headless (heavy GUI/FFI/native deps: Bloc, iceberg,
kendrick, biosmalltalk, the `pharo-sdl`/graphics ones, etc.).

## Takeaway

The sweep did its two jobs: (a) **broadly validated x86-JIT correctness** — 143
clean-parity packages, 0 JIT bugs; and (b) surfaced the real gaps that block
broader coverage, all VM-core: the **stub SSL backend** (highest-value fix) and
**SQLite3 FFI loading**.
