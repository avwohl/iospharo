# JIT Toolkit Evaluation

**Context:** Our current T2 JIT uses MIR. MIR's ARM64 codegen has measurable
weaknesses (no immediate-offset LDR/STR, sp spilling across branches,
movk chains for every SmallInt literal) that make T2 slower than T1
on arith loops (12× slower than interp for `sum 3M`). This doc
surveys alternatives given the target platform matrix.

## Targets

Mandatory support:
-   macOS     x86_64 + arm64 (Apple Silicon Macs)
-   Linux     x86_64 + aarch64
-   Windows   x86_64 + aarch64 (ARM Windows)

Nice to have:
-   iOS       arm64 (already works via interpreter; JIT on iOS has
              W^X signing hurdles)
-   BSD       x86_64 + aarch64

## What we need from a JIT toolkit

A Smalltalk JIT is **send-heavy** and **register-pressure-light**.
The dominant operations are:
- Oop tagging/untagging (shift + OR)
- Stack push/pop (memory ops against our Smalltalk stack)
- Arith with overflow detection (ADDO/SUBO on ARM64, branch-on-overflow)
- Indirect calls through ICs (C function call or direct thunk)
- Compare + branch to true/false Oops

Traits that matter for us:
1. **Immediate-offset addressing** — we access state-struct fields constantly
2. **Callee-saved registers** — we want sp/receiver/tempBase to stay resident
3. **Fast compile time** — we compile hot methods at runtime, can't afford 500ms
4. **Small runtime** — we embed this in every Pharo image
5. **Cross-platform x64 + arm64** — Mac/Linux/Windows all covered
6. **C or C++ friendly** — our VM is C++; Rust bridging is a tax

## Toolkit survey

### 1. MIR  *(current)*

- **Language:** C.
- **Size:** ~100 KB compiled.
- **Archs:** x86_64, aarch64, ppc64, s390x, riscv64.
- **Platforms:** Mac, Linux, Windows, BSD.
- **Compile speed:** fast (few ms per function).
- **Codegen quality:** mediocre. ARM64 specifics we've hit:
    - `ldr` always uses indexed `[reg, xzr, lsl #3]` form, never immediate-offset.
    - `reg_sp_` and similar caller-saved across branches is spilled to memory.
    - No literal pool for tagged 48-bit immediates — three movk per use.
    - Level 2/3 optimization either no help or regresses.
- **SSA IR:** yes.
- **Verdict:** we already have it; ARM64 backend is the blocker.

### 2. LLVM  (via ORC JIT or LLJIT)

- **Language:** C++.
- **Size:** huge — 30+ MB of libraries embedded, 100+ MB of headers at build.
- **Archs:** everything.
- **Platforms:** everything.
- **Compile speed:** slow — 50-500 ms per function depending on optimization.
- **Codegen quality:** best-in-class.
- **Integration:** good C++ bindings; supported by Apple/Google/Microsoft.
- **Verdict:** amazing codegen at the price of a heavy dependency and
  slow per-method compile time. For a *hot method* tier where a method
  compiles once and runs millions of times, the compile-time cost
  amortizes. But embedded-VM-shaped concerns (startup time, size)
  argue against. Also careful with LLVM version skew across platforms.

### 3. Cranelift

- **Language:** Rust.
- **Size:** ~5 MB compiled (without LLVM).
- **Archs:** x86_64, aarch64, riscv64, s390x.
- **Platforms:** Mac, Linux — Windows **aarch64 is partial** (x86_64 is
  fine on Windows).
- **Compile speed:** fast (tens of ms).
- **Codegen quality:** very good. Designed for WebAssembly JIT, so
  register allocation and instruction selection are tuned for this
  exact use case.
- **Integration:** Rust native. `cranelift-c` C bindings exist but are
  a bolt-on. For a C++ project, we'd either embed a Rust crate (adds
  Cargo to our build) or write a thin Rust wrapper around Cranelift +
  expose a C ABI we call from C++.
- **Verdict:** technically the best match for our workload, but Rust
  integration is friction. Useful if the perf win is large enough.

### 4. libgccjit  (GCC's JIT backend)

- **Language:** C.
- **Size:** pulls in most of GCC — tens of MB.
- **Archs:** everything GCC supports.
- **Platforms:** Linux native; Mac via Homebrew GCC; Windows via MSYS2.
  Not a native Windows-MSVC citizen.
- **Compile speed:** slow — comparable to LLVM.
- **Codegen quality:** excellent (GCC's actual backend).
- **Licensing:** GPL runtime libraries — Smalltalk image may need
  compatible licensing. (Our image is MIT — problematic.)
- **Verdict:** skip — licensing + Windows-native issues.

### 5. asmjit  (C++ assembler library)

- **Language:** C++, permissive license.
- **Size:** ~500 KB.
- **Archs:** x86, x86_64, aarch32, aarch64, riscv64.
- **Platforms:** Mac/Linux/Windows fully supported.
- **Compile speed:** very fast.
- **Codegen quality:** you write the asm. No register allocation, no
  optimization. What you write is what you get.
- **Integration:** C++ native, easy.
- **Verdict:** the right tool if we want to write Cog-style per-arch
  assembly. No automatic optimization means we write each arch by
  hand — 2 arches × several dozen stencils + whole-method logic.
  A multi-week project per arch. BUT every cycle is ours to control.

### 6. DynASM  (Mike Pall's, used by LuaJIT, MoarVM)

- **Language:** Lua-macro preprocessor emitting C.
- **Size:** tiny.
- **Archs:** x86, x86_64, arm, arm64, ppc, mips, s390x (LuaJIT
  maintained set, quality varies).
- **Platforms:** whatever C you compile on.
- **Compile speed:** essentially zero (all at build time).
- **Codegen quality:** hand-written by you; Mike Pall's examples are
  world-class.
- **Integration:** build-time preprocessor, C output. Unusual build
  setup but well-trodden.
- **Verdict:** what LuaJIT and MoarVM use. Similar trade to asmjit:
  you write per-arch, you get every cycle. But the tooling is
  Lua-dependent and less C++-friendly than asmjit.

### 7. GNU Lightning

- **Language:** C.
- **Size:** small.
- **Archs:** x86_64, aarch64, arm, mips, ppc, s390x, riscv.
- **Platforms:** Linux good, Mac okay, Windows poor.
- **Compile speed:** fast.
- **Codegen quality:** below MIR, limited optimization.
- **Verdict:** skip. Older, less active, worse than MIR.

### 8. Copy-and-patch stencils *(our T1, extended)*

- **Language:** whatever produces the stencils (we use clang on C).
- **Size:** just the stencils + patcher; ~50 KB of infrastructure.
- **Archs:** wherever clang compiles — x86_64, aarch64, etc.
- **Platforms:** Mac/Linux/Windows (need Windows build of
  extract_stencils.py).
- **Compile speed:** instant (copy-and-patch is essentially memcpy +
  a few relocations).
- **Codegen quality:** whatever clang emitted for our C source —
  good, but per-bytecode stencil boundaries cost.
- **Integration:** already in the codebase.
- **Verdict:** this is our T1. Not a *toolkit* per se but an
  architecture. Extending to better whole-method codegen is a large
  redesign.

## Matrix

    Tool          Quality  Speed  Size   C++?   x64  arm64  Mac  Lin  Win
    MIR*          fair     fast   small  yes    ✓    weak   ✓   ✓    ✓
    LLVM          best     slow   huge   yes    ✓    ✓      ✓   ✓    ✓
    Cranelift     great    fast   small  Rust*  ✓    ✓      ✓   ✓    partial
    libgccjit     great    slow   big    yes    ✓    ✓      ✓   ✓    MSYS
    asmjit        hand     fast   small  yes    ✓    ✓      ✓   ✓    ✓
    DynASM        hand     fast   tiny   yes*   ✓    ✓      ✓   ✓    ✓
    GNU Lightning poor     fast   small  yes    ✓    ✓      ✓   ✓    weak
    Copy-patch    fair     best   tiny   yes    ✓    ✓      ✓   ✓    need-build

*MIR ARM64 is fair overall but has the specific issues we hit. *Cranelift needs
Rust toolchain in build. *DynASM needs Lua for the preprocessor (build-time only).

## Recommendation

### Short term (1-2 sessions, low risk)

**Fix MIR's ARM64 backend** where we can. Specifically:
- Patch the instruction selector to emit `ldr Rd, [Rn, #imm]` when the
  displacement fits. This is a targeted change to MIR's `target_` layer.
- Submit upstream; maintain as a small fork in `third_party/mir/` with
  our patches on top until merged.

This is probably a week of focused MIR hacking but gives us Cog-quality
addressing without changing our codegen strategy. Relative perf win:
maybe 30-40% on the T2 hot path.

### Medium term (3-5 sessions)

**Add asmjit as an alternative backend for T2 on hot methods.**
Keep MIR for "good enough" compile. For methods that get REALLY hot,
re-emit through asmjit with hand-tuned sequences for the critical
path (arith fast paths, send IC checks, loop back-edges). Cog-style
per-arch assembly for the 20% of methods that matter.

### Long term (multi-session)

**Migrate T2 to Cranelift** via a thin Rust shim. Accept the Rust
build dependency as the price of better codegen on ARM64 + Windows.
Cranelift is what Wasmtime and Firefox actually use for JIT in
production on all our target platforms.

Or, alternatively, **commit to Cog-style everywhere** by replacing
T2 with a dedicated asmjit-based optimizing tier. Keep T1 copy-patch
for cold methods. This is what Cog, JavaScriptCore, and V8's baseline
all look like: per-arch hand-assembly for the hot path, optimizer
tier only for the hottest.

## My vote

1. First, spend a week on MIR ARM64. If we can fix immediate-offset
   addressing and the sp-spill behavior, we keep our current toolkit
   and gain perhaps 30-40% on T2.
2. If that's not enough to hit Cog parity, add asmjit for hot-method
   code paths. asmjit is the most C++-friendly, least-intrusive way
   to write cross-platform hand-assembly.
3. Cranelift is the strongest technical choice but the Rust bridge
   is a real cost — only take that step if asmjit hand-assembly
   proves infeasible to maintain.

Avoid: LLVM (too heavy), libgccjit (licensing/Windows), Lightning
(worse than MIR), DynASM (Lua dependency).

Skip the "write from scratch" path for now — asmjit gets you
everything write-from-scratch gets plus cross-platform assembler
infrastructure already debugged.
