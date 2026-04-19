/*
 * test_sista_ir.cpp - Minimal self-contained test for SistaIR.
 *
 * Builds a trivial method's IR manually (no lifter yet), runs basic
 * consistency checks, and dumps the textual form.  Regression gate
 * against IR bit-rot while the rest of Phase 2 is under construction.
 *
 * Corresponds to the method:
 *
 *   Integer>>double
 *     ^ self + self
 *
 * Which in SmallInt-fast-path IR is:
 *
 *   block 0:
 *     v0 : Oop         = load_recv
 *     v1 : Int         = tag_check_int v0
 *     v2 : Int         = prim_add_int v1 v1
 *     v3 : OopSmallInt = const_oop  (int-to-oop wrap, TODO)
 *     return v3
 *
 *   (The int→oop wrap is a placeholder — the real codegen will
 *   bit-shift the raw int and set the SmallInt tag.  The IR only
 *   needs to carry the intent.)
 */
#include "SistaIR.hpp"
#include "SistaBuilder.hpp"
#include "SistaLowering.hpp"
#include "../SistaV1.hpp"

#include <cassert>
#include <cstdio>
#include <iostream>

using namespace pharo::sista;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    Method m;
    m.numArgs  = 0;
    m.numTemps = 0;

    uint32_t entry = m.newBlock(/*sourceBc=*/0);
    m.entryBlock = entry;

    uint32_t v0 = m.newValue(entry, Op::kLoadReceiver, Type::kOop);
    uint32_t v1 = m.newValue(entry, Op::kPrimTagCheckInt, Type::kInt, {v0});
    uint32_t v2 = m.newValue(entry, Op::kPrimAddInt,      Type::kInt, {v1, v1});
    // Synthetic int→Oop — we'll replace with a real op when we have one.
    uint32_t v3 = m.newValue(entry, Op::kConstantOop, Type::kOopSmallInt, {}, 0);
    m.newValue(entry, Op::kReturn, Type::kVoid, {v3});

    // ===== Consistency checks =====
    check(m.blocks.size() == 1, "one block");
    check(m.values.size() == 5, "five values");
    check(OpInfo::isTerminator(Op::kReturn), "return is terminator");
    check(!OpInfo::isTerminator(Op::kPrimAddInt), "add is not terminator");
    check(OpInfo::producesOop(Op::kLoadReceiver), "load_recv is an oop");
    check(!OpInfo::producesOop(Op::kPrimAddInt), "add_int is not an oop");
    check(OpInfo::mayCallBack(Op::kSendUnspeculated), "send may deopt");
    check(!OpInfo::mayCallBack(Op::kPrimAddInt), "add_int stays local");

    // Entry block has right value ids
    const Block& b = m.blockAt(entry);
    check(b.values.size() == 5, "entry has 5 values");
    check(b.values[0] == v0, "v0 first");
    check(b.values[4] == 4,  "terminator last");

    // Types propagated
    check(m.valueAt(v0).type == Type::kOop,  "v0 typed Oop");
    check(m.valueAt(v1).type == Type::kInt,  "v1 typed Int");
    check(m.valueAt(v2).type == Type::kInt,  "v2 typed Int");

    // Textual dump — sanity
    std::string dump = m.toString();
    check(dump.find("load_recv") != std::string::npos, "dump mentions load_recv");
    check(dump.find("prim_add_int") != std::string::npos, "dump mentions add");
    check(dump.find("return") != std::string::npos, "dump mentions return");

    std::cout << dump;

    // ===== Lifter tests =====
    using namespace pharo::jit;

    // Integer>>yourself ≡ [PushReceiver, ReturnTop]
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,
            SistaV1::ReturnTop,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc),
                                                 0, 0, lifted, &failed);
        check(r == LiftResult::kOk, "yourself lifts");
        check(lifted.blocks.size() == 1, "one block");
        // Expected: load_recv, return
        check(lifted.values.size() == 2, "two IR values for yourself");
        check(lifted.valueAt(0).op == Op::kLoadReceiver, "v0 load_recv");
        check(lifted.valueAt(1).op == Op::kReturn, "v1 return");
        check(lifted.valueAt(1).operands.size() == 1 &&
              lifted.valueAt(1).operands[0] == 0, "return feeds v0");
        std::cout << "\n--- lifted yourself ---\n" << lifted.toString();
    }

    // Integer>>yourselfViaReturn ≡ [ReturnReceiver]
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::ReturnReceiver };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc),
                                                 0, 0, lifted);
        check(r == LiftResult::kOk, "return-receiver lifts");
        check(lifted.values.size() == 2, "2 values: load_recv + return");
        check(lifted.valueAt(1).op == Op::kReturn, "return terminator");
        std::cout << "\n--- lifted ^ self ---\n" << lifted.toString();
    }

    // Pushtemp, ReturnTop ≡ [PushTemp 3, ReturnTop]
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushTempBase + 3),
            SistaV1::ReturnTop,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc),
                                                 0, 4, lifted);
        check(r == LiftResult::kOk, "pushTemp/return lifts");
        check(lifted.valueAt(0).op == Op::kLoadTemp, "load_temp");
        check(lifted.valueAt(0).literal == 3, "temp idx 3");
        std::cout << "\n--- lifted ^ t3 ---\n" << lifted.toString();
    }

    // Unsupported bytecode bails cleanly
    {
        Method lifted;
        const uint8_t bc[] = { 0xAA };  // arbitrary unmapped op
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc),
                                                 0, 0, lifted, &failed);
        check(r == LiftResult::kUnsupportedBytecode, "bail on unsupported");
        check(failed == 0, "fails at offset 0");
    }

    // Lowering round-trip: lift → lower → execute → verify.
    // Minimal JITState stand-in matching the real layout (offsets from
    // src/vm/jit/JITState.hpp).  Only the fields the minimal subset
    // touches are set.
    struct FakeState {
        void*    sp;              // offset 0
        uint64_t receiver;        // offset 8
        void*    literals;        // offset 16
        void*    tempBase;        // offset 24
        uint8_t  padA[40];        // fill to offset 72
        int      argCount;        // offset 72
        int      exitReason;      // offset 76
        uint64_t returnValue;     // offset 80
        uint8_t  padB[256];       // slack
    };
    static_assert(offsetof(FakeState, receiver) == 8, "receiver offset");
    static_assert(offsetof(FakeState, tempBase) == 24, "tempBase offset");
    static_assert(offsetof(FakeState, exitReason) == 76, "exitReason offset");
    static_assert(offsetof(FakeState, returnValue) == 80, "returnValue offset");

    Lowering lowering;

    // Round-trip 1: Integer>>yourself (^ self).
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::PushReceiver, SistaV1::ReturnTop };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        uint32_t failed = UINT32_MAX;
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower yourself succeeds");

        FakeState state{};
        state.receiver = 0xCAFEBABEULL;   // Fake Oop pattern
        fn(&state);
        check(state.returnValue == 0xCAFEBABEULL, "returnValue = receiver");
        check(state.exitReason == 1, "exitReason = EXIT_RETURN");
        std::cout << "\n--- round-trip yourself: returnValue=0x" << std::hex
                  << state.returnValue << std::dec
                  << " exitReason=" << state.exitReason << "\n";
    }

    // Round-trip 2: ^ t3 (PushTemp 3, ReturnTop).
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushTempBase + 3),
            SistaV1::ReturnTop,
        };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 4, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ^ t3 succeeds");

        uint64_t temps[4] = { 0x1111, 0x2222, 0x3333, 0xDEADBEEF };
        FakeState state{};
        state.tempBase = temps;
        fn(&state);
        check(state.returnValue == 0xDEADBEEFULL, "returnValue = temp[3]");
        check(state.exitReason == 1, "exitReason = EXIT_RETURN");
        std::cout << "--- round-trip ^ t3: returnValue=0x" << std::hex
                  << state.returnValue << std::dec << "\n";
    }

    // Round-trip 3: ^ self via ReturnReceiver bytecode (same result).
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::ReturnReceiver };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower return-receiver succeeds");

        FakeState state{};
        state.receiver = 0x5555666677778888ULL;
        fn(&state);
        check(state.returnValue == 0x5555666677778888ULL, "returnValue = receiver");
        std::cout << "--- round-trip return-receiver: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    std::cout << "PASS\n";
    return 0;
}
