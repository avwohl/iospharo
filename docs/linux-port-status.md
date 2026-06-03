# x86_64 / Linux build status

Status of building the clean C++ VM (with JIT) for **x86_64 Linux**. The arm64
JIT redesign happens on `jit`; this x86 work lives on `jit-x86` and is merged
back into `jit` deliberately (never pushed straight onto it).

## Build environment

Built on a disposable AWS EC2 **spot** instance — see `scripts/aws/` (README
there for the full runbook). Ubuntu 24.04, gcc 13 / libstdc++, 8 vCPU / 32 GiB.

    ./scripts/build-linux.sh                 # Release, JIT on, crypto off
    PHARO_DISABLE_LTO=1 ./scripts/build-linux.sh   # fast, non-LTO (default on the spot box)

Produces `build/test_load_image` (x86-64 ELF). LTO off is the spot-box default:
a ~3-5 min build finishes inside a spot lifetime; a full LTO build (~15 min)
risks reclaim mid-link.

## x86_64 build fixes (compiles + links clean as of jit-x86)

These three arm64-isms broke the x86_64/libstdc++ build; each is fixed by arch
guard or a portable equivalent (not a workaround — same behavior per arch):

    src/vm/no_getenv.hpp        the force-included `#define getenv X` poison broke
                                libstdc++'s `using ::getenv;` ("not declared in
                                '::'"). Fix: include <cstdlib>/<stdlib.h> first,
                                then poison — ban still fires on call sites.
    src/vm/ObjectMemory.cpp     readTSC() used ARM `mrs cntvct_el0`. Fix: arch
                                guard — rdtsc on x86, mrs on arm, chrono else.
    .../jit/Tier2Compiler_x86_64.cpp   bare `trace` undeclared at two trace
                                sites. Fix: alias to g_debug.t2X86Trace.

## Known WIP / deferred

    Sista x86_64 lowering   defaults OFF on x86 (DebugSettings: kDefaultSistaOn
                            = false unless __aarch64__). The lowering path still
                            emits asmjit::a64 on every host; running Sista on x86
                            faults. PHARO_SISTA_DISPATCH=1 forces it on for
                            bisecting the x86 lowering work.
    %lld format warnings    ~33 -Wformat warnings: `%lld` vs int64_t, which is
                            `long` on x86_64 (not `long long` as on arm64 macOS).
                            Non-blocking (no -Werror). Fix later with PRId64.
    Tier2Compiler_x86_64    pattern-matching tier-2 compiler is in progress
                            (compiles; runtime correctness is the next milestone).

## Next steps

1. Runtime smoke test: `test_load_image <Pharo.image>` loads + runs interpreter.
2. Bring the x86 JIT (T1 stencils + T2) to runtime parity; enable Sista on x86.
3. Clean up the `%lld`/int64_t format warnings (PRId64).
