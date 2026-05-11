/*
 * test_asmjit_t1_stub.cpp - Phase 0 proof of concept for the
 * fuzzer-then-asmjit JIT rebuild plan
 * (scripts/jit-diff/plan_asmjit_replacement.md).
 *
 * Goal: prove that asmjit can emit a `void fn(JITState*)` function
 * that the existing JIT_CALL macro can invoke, and that the function
 * can write `state->exitReason = ExitSend` per the JITState layout
 * defined in JITState.hpp.
 *
 * If this works on x86_64 and arm64, Phase 1 of the rebuild can wire
 * the same pattern into JITCompiler::compile behind a
 * PHARO_USE_ASMJIT_T1=1 env flag.
 *
 * No integration with the existing JIT runtime — this test runs
 * standalone, allocates its own JITState on the stack, and verifies
 * the field write.
 *
 * Build & run:
 *   cmake --build build --target test_asmjit_t1_stub
 *   ./build/test_asmjit_t1_stub
 * Exit 0 on success, non-zero on any failure.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <asmjit/x86.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <asmjit/a64.h>
#else
#error "Unsupported architecture for asmjit T1 stub"
#endif

#include <asmjit/core/jitruntime.h>

// JITState field offsets — must match src/vm/jit/JITState.hpp.
// Hard-coded here to keep this test standalone (no PharoVMCore link).
// The static_asserts at the bottom of JITState.hpp guarantee these
// offsets are stable.
static constexpr int OFF_EXIT = 76;
static constexpr int EXIT_SEND = 2;

// Mock state struct just big enough that offset 76 is valid.  We
// don't reproduce the full JITState here because Phase 0 only writes
// one field; integration into the real runtime will use the actual
// JITState struct.
struct MockJITState {
    uint8_t pad[OFF_EXIT];
    int exitReason;
    uint8_t tail[200];
};
static_assert(offsetof(MockJITState, exitReason) == OFF_EXIT,
              "MockJITState exitReason offset matches JITState");

// asmjit-emit a function:
//   void fn(MockJITState* state) {
//       state->exitReason = EXIT_SEND;
//       return;
//   }
// Returns the emitted function pointer, or nullptr on failure.
typedef void (*StubFn)(MockJITState*);

static StubFn emitStub(asmjit::JitRuntime& rt) {
    using namespace asmjit;

    CodeHolder code;
    Error err = code.init(rt.environment(), rt.cpu_features());
    if (err != asmjit::kErrorOk) {
        std::fprintf(stderr, "code.init failed: %s\n", DebugUtils::error_as_string(err));
        return nullptr;
    }

#if defined(__x86_64__) || defined(_M_X64)
    x86::Assembler a(&code);
    // SysV AMD64: arg0 = rdi.  Win64 would use rcx (we're not on Windows).
    // mov DWORD PTR [rdi + 76], 2
    a.mov(x86::dword_ptr(x86::rdi, OFF_EXIT), Imm(EXIT_SEND));
    a.ret();
#elif defined(__aarch64__) || defined(_M_ARM64)
    a64::Assembler a(&code);
    // AArch64: arg0 = x0.  exitReason is int32_t — w1 = 2; str w1, [x0, #76].
    a64::Gp w1 = a64::w1;
    a.mov(w1, Imm(EXIT_SEND));
    a.str(w1, a64::ptr(a64::x0, OFF_EXIT));
    a.ret(a64::x30);
#endif

    StubFn fn = nullptr;
    err = rt.add(&fn, &code);
    if (err != asmjit::kErrorOk) {
        std::fprintf(stderr, "rt.add failed: %s\n", DebugUtils::error_as_string(err));
        return nullptr;
    }
    return fn;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    asmjit::JitRuntime runtime;

    StubFn fn = emitStub(runtime);
    if (!fn) {
        std::fprintf(stderr, "FAIL: stub emission returned nullptr\n");
        return 1;
    }
    std::printf("emitted stub at %p\n", (void*)fn);

    // Call it on a zeroed state; verify exitReason flips to 2.
    MockJITState state;
    std::memset(&state, 0, sizeof(state));
    if (state.exitReason != 0) {
        std::fprintf(stderr, "FAIL: pre-call exitReason=%d (expected 0)\n",
                     state.exitReason);
        return 1;
    }

    fn(&state);

    if (state.exitReason != EXIT_SEND) {
        std::fprintf(stderr,
                     "FAIL: post-call exitReason=%d (expected %d)\n",
                     state.exitReason, EXIT_SEND);
        return 1;
    }
    std::printf("PASS: state.exitReason = %d (= EXIT_SEND)\n",
                state.exitReason);

    // Run it 1000x to verify the emitted code is reusable / not a
    // one-shot.
    for (int i = 0; i < 1000; i++) {
        state.exitReason = 0;
        fn(&state);
        if (state.exitReason != EXIT_SEND) {
            std::fprintf(stderr, "FAIL: iter %d gave exitReason=%d\n",
                         i, state.exitReason);
            return 1;
        }
    }
    std::printf("PASS: 1000 reentrant calls all wrote EXIT_SEND\n");

    runtime.release(fn);
    return 0;
}
