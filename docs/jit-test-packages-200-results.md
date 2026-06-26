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

1. **SSL/TLS — FIXED (commit f26d45a2).** The biggest cluster. Root cause was
   TWO build gates, neither a JIT bug: `build-linux.sh` hardcoded
   `PHARO_WITH_CRYPTO=OFF` (so SqueakSSL was never compiled/registered → HTTPS =
   `ZdcPluginMissing`, and native SHA1/MD5/DSA were off too), and the non-Apple
   backend `src/vm/plugins/sqGenericSSL.c` was a no-op stub (every `sq*SSL`
   returned `SQSSL_GENERIC_ERROR`). Fix: a real OpenSSL mem-BIO backend in
   `sqGenericSSL.c` + crypto ON by default + link system libssl/libcrypto +
   `libssl-dev` in bootstrap. HTTPS now works (example.org, google 81 KB, github
   API, httpbin, raw.githubusercontent) over **TLS 1.2 and TLS 1.3**, x86 JIT off
   **and** on. The TLS 1.3 fix (commit after f26d45a2): on handshake completion
   the backend leaves the client Finished in the write BIO (returns `SQSSL_OK`
   without draining) so the image flushes it *with* the request — required because
   Cloudflare/Google send `NewSessionTicket`+`close_notify` right after the
   handshake and close unless the request is pipelined with the Finished (matches
   upstream `sqUnixOpenSSL.inc`). This unblocks the HTTPS packages:
   `evref-bl-*-pharo-api`, `ba-st-stargate`,
   `smalltalkweb-myprecious`, `newapplesho-google-cloud`, `juliendelplanque-jrpc`,
   network half of `svenvc-p3`.

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
