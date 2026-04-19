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
    check(OpInfo::producesOop(Op::kPrimAddInt),
          "add_int produces tagged SmallInt Oop");
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

    // Unsupported bytecode bails cleanly.
    // Pick an op that is NOT a send / push / store / branch / arith —
    // 0xEA (ExtSend) requires prefix handling we haven't wired up.
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::ExtSend };
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

    // FakeState with trueOop / falseOop slots at the right offsets
    // (must match OFF_TRUEOOP = 128, OFF_FALSEOOP = 136).
    struct FakeStateWithBools {
        void*    sp;
        uint64_t receiver;
        void*    literals;
        void*    tempBase;
        uint8_t  padA[40];
        int      argCount;
        int      exitReason;
        uint64_t returnValue;
        uint8_t  padB[40];
        uint64_t trueOop;
        uint64_t falseOop;
        uint8_t  padC[64];
    };
    static_assert(offsetof(FakeStateWithBools, trueOop) == 128,  "trueOop offset");
    static_assert(offsetof(FakeStateWithBools, falseOop) == 136, "falseOop offset");

    // Round-trip 4: ^ true (PushTrue, ReturnTop).
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::PushTrue, SistaV1::ReturnTop };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ^ true succeeds");

        FakeStateWithBools state{};
        state.trueOop  = 0xA0A0A0A0U;
        state.falseOop = 0xB0B0B0B0U;
        fn(&state);
        check(state.returnValue == 0xA0A0A0A0U, "returnValue = trueOop");
        std::cout << "--- round-trip ^ true: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 5: ReturnFalse bytecode (single-byte).
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::ReturnFalse };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ReturnFalse succeeds");

        FakeStateWithBools state{};
        state.trueOop  = 0xA0A0A0A0U;
        state.falseOop = 0xB0B0B0B0U;
        fn(&state);
        check(state.returnValue == 0xB0B0B0B0U, "returnValue = falseOop");
        std::cout << "--- round-trip ReturnFalse: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 6: ^ 0 (PushZero, ReturnTop) → SmallInt 0 bits = 1.
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::PushZero, SistaV1::ReturnTop };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ^ 0 succeeds");

        FakeState state{};
        fn(&state);
        check(state.returnValue == 1ULL, "SmallInt 0 bits = 1");
        std::cout << "--- round-trip ^ 0: returnValue=0x"
                  << std::hex << state.returnValue << std::dec
                  << " (SmallInt 0)\n";
    }

    // Round-trip 7: ^ 1 (PushOne, ReturnTop) → SmallInt 1 bits = 9.
    {
        Method lifted;
        const uint8_t bc[] = { SistaV1::PushOne, SistaV1::ReturnTop };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ^ 1 succeeds");

        FakeState state{};
        fn(&state);
        check(state.returnValue == 9ULL, "SmallInt 1 bits = 9");
        std::cout << "--- round-trip ^ 1: returnValue=0x"
                  << std::hex << state.returnValue << std::dec
                  << " (SmallInt 1)\n";
    }

    // Round-trip 7.5: ^ instVar[1] — PushRecvVar 1, ReturnTop.
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushRecvVarBase + 1),
            SistaV1::ReturnTop,
        };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ^ ivar[1] succeeds");

        // Lay out a fake object with 3 slots.  Slot 0 at +8,
        // slot 1 at +16, slot 2 at +24.
        uint64_t obj[4] = {
            0xDEADDEAD,   // header (ignored by load)
            0x1111,       // slot 0
            0xABCD,       // slot 1 — expected return
            0x2222,       // slot 2
        };
        FakeState state{};
        state.receiver = reinterpret_cast<uint64_t>(&obj[0]);
        fn(&state);
        check(state.returnValue == 0xABCDULL, "returnValue = ivar[1]");
        std::cout << "--- round-trip ^ ivar[1]: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.6: PushZero, PopStoreTemp 0, PushOne, PopStoreTemp 0,
    // PushTemp 0, ReturnTop — stores-then-reads, ensure store is live.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushZero,
            (uint8_t)(SistaV1::PopStoreTempBase + 0),
            SistaV1::PushOne,
            (uint8_t)(SistaV1::PopStoreTempBase + 0),
            (uint8_t)(SistaV1::PushTempBase + 0),
            SistaV1::ReturnTop,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 1, lifted, &failed);
        check(r == LiftResult::kOk, "store+read temp lifts");

        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower store+read succeeds");

        uint64_t temps[1] = { 0xDEAD };
        FakeState state{};
        state.tempBase = temps;
        fn(&state);
        check(state.returnValue == 9ULL, "read-back temp == SmallInt 1 bits");
        check(temps[0] == 9ULL, "store landed in temp[0]");
        std::cout << "--- round-trip store+read temp: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.7: Pop/Dup — push, dup, pop, pop — stack accounting.
    // Method: ^ (PushOne, Dup, Pop) = ^ 1.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushOne,
            SistaV1::Dup,
            0xD8,                 // Pop
            SistaV1::ReturnTop,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);
        check(r == LiftResult::kOk, "dup/pop lifts");

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower dup/pop");

        FakeState state{};
        fn(&state);
        check(state.returnValue == 9ULL, "PushOne/Dup/Pop/Ret yields 1");
    }

    // Round-trip 7.8: Setter — PushTemp 0, PopStoreRecvVar 1, ReturnReceiver.
    // Simulates `x: aValue  x := aValue. ^ self`.
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushTempBase + 0),       // load arg (temp 0)
            (uint8_t)(SistaV1::PopStoreRecvBase + 1),   // store to ivar[1]
            SistaV1::ReturnReceiver,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 1, 1, lifted);
        check(r == LiftResult::kOk, "setter lifts");

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower setter succeeds");

        uint64_t obj[4]   = { 0xDEADDEAD, 0x1111, 0xBEEF, 0x3333 };
        uint64_t temps[1] = { 0xFACADE };
        FakeState state{};
        state.receiver = reinterpret_cast<uint64_t>(&obj[0]);
        state.tempBase = temps;
        fn(&state);
        check(obj[2] == 0xFACADE, "ivar[1] now holds arg value");
        check(state.returnValue == reinterpret_cast<uint64_t>(&obj[0]),
              "return value = receiver");
        std::cout << "--- round-trip setter: ivar[1]=0x"
                  << std::hex << obj[2] << std::dec << "\n";
    }

    // Round-trip 7.9: Multi-block — ^ (cond ifTrue: [1] ifFalse: [0]).
    // Pattern:
    //   0: PushTrue      (condition)
    //   1: JumpIfFalse +2  (0xC1 → target bc[4])
    //   2: PushOne
    //   3: ReturnTop
    //   4: PushZero
    //   5: ReturnTop
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushTrue,
            0xC1,                     // jumpIfFalse +2 → target = 0+1+1+1 = …
                                       // shortJumpTarget(1, 0xC1) = 1 + 1 + 2 = 4
            SistaV1::PushOne,
            SistaV1::ReturnTop,
            SistaV1::PushZero,
            SistaV1::ReturnTop,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted, &failed);
        check(r == LiftResult::kOk, "multi-block lifts");
        check(lifted.blocks.size() >= 3,
              "at least entry + taken + fallthrough blocks");
        std::cout << "\n--- lifted true ifTrue:[1] ifFalse:[0] ---\n"
                  << lifted.toString();

        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower multi-block");

        FakeStateWithBools state{};
        state.trueOop  = 0x1111;
        state.falseOop = 0x2222;

        // cond=true path: expect SmallInt 1 bits = 9.
        fn(&state);
        check(state.returnValue == 9ULL,
              "true path returns SmallInt 1");

        // cond=false path: set receiver so PushTrue loads a different
        // value... wait, we load from state->trueOop.  To test the
        // "false" branch we'd need a different bytecode sequence with
        // PushFalse instead.  Check that works too.
        {
            Method lifted2;
            const uint8_t bc2[] = {
                SistaV1::PushFalse,
                0xC1,
                SistaV1::PushOne,
                SistaV1::ReturnTop,
                SistaV1::PushZero,
                SistaV1::ReturnTop,
            };
            Builder::buildFromBytes(bc2, sizeof(bc2), 0, 0, lifted2);
            Lowering::CompiledFn fn2 = lowering.lower(lifted2);
            check(fn2 != nullptr, "lower false-branch variant");
            FakeStateWithBools s{};
            s.trueOop  = 0x1111;
            s.falseOop = 0x2222;
            fn2(&s);
            check(s.returnValue == 1ULL,
                  "false path returns SmallInt 0");
        }
        std::cout << "--- multi-block cond test: ok (both branches)\n";
    }

    // Round-trip 7.A: Arith — `^ 1 + 1` → SmallInt 2 (bits = 17).
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushOne,
            SistaV1::PushOne,
            (uint8_t)(SistaV1::ArithBase + 0),   // +
            SistaV1::ReturnTop,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);
        check(r == LiftResult::kOk, "1 + 1 lifts");

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower 1 + 1");

        FakeState state{};
        fn(&state);
        // SmallInt 2 bits = (2 << 3) | 1 = 17 = 0x11
        check(state.returnValue == 0x11ULL, "1 + 1 = 2 (tagged 0x11)");
        std::cout << "--- round-trip 1 + 1: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.B: Arith — `^ (temp0 + temp1) - 1`.
    //   PushTemp 0, PushTemp 1, +, PushOne, -, ReturnTop
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushTempBase + 0),
            (uint8_t)(SistaV1::PushTempBase + 1),
            (uint8_t)(SistaV1::ArithBase + 0),
            SistaV1::PushOne,
            (uint8_t)(SistaV1::ArithBase + 1),
            SistaV1::ReturnTop,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 2, lifted);
        check(r == LiftResult::kOk, "(t0 + t1) - 1 lifts");

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower (t0 + t1) - 1");

        // temp0 = SmallInt 10 (bits 0x51), temp1 = SmallInt 3 (bits 0x19)
        // (10 + 3) - 1 = 12 → SmallInt bits = (12 << 3) | 1 = 97 = 0x61
        uint64_t temps[2] = {
            (10ULL << 3) | 1,
            ( 3ULL << 3) | 1,
        };
        FakeState state{};
        state.tempBase = temps;
        fn(&state);
        check(state.returnValue == 0x61ULL,
              "(10 + 3) - 1 = 12 (tagged 0x61)");
        std::cout << "--- round-trip (t0 + t1) - 1: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.C: Multiply — `^ temp0 * temp1`.
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushTempBase + 0),
            (uint8_t)(SistaV1::PushTempBase + 1),
            (uint8_t)(SistaV1::ArithBase + 8),   // *
            SistaV1::ReturnTop,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 2, lifted);
        check(r == LiftResult::kOk, "t0 * t1 lifts");

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower t0 * t1");

        // 7 * 6 = 42 → SmallInt bits = (42 << 3) | 1 = 337 = 0x151
        uint64_t temps[2] = {
            (7ULL << 3) | 1,
            (6ULL << 3) | 1,
        };
        FakeState state{};
        state.tempBase = temps;
        fn(&state);
        check(state.returnValue == 0x151ULL, "7 * 6 = 42");
        std::cout << "--- round-trip 7 * 6: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.D: Unspeculated send — `self foo: arg` bails to
    // interpreter at the send bytecode.  Pattern:
    //   0: PushReceiver
    //   1: PushTemp 0     (one arg)
    //   2: Send1 sel=3    (0x93)
    // We verify exitReason = ExitSend (2), sp advanced by 16 bytes
    // (rcvr + 1 arg), and sendArgCount = 1.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,
            (uint8_t)(SistaV1::PushTempBase + 0),
            (uint8_t)(SistaV1::Send1Base + 3),   // 0x93: send lit[3], 1 arg
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 1, 1,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "send1 lifts");
        check(lifted.values.size() == 3,
              "3 IR values: rcvr, arg, kSendUnspeculated");
        check(lifted.valueAt(2).op == Op::kSendUnspeculated,
              "last value is send");
        check(lifted.valueAt(2).operands.size() == 2,
              "send has {rcvr, arg}");
        // literal packs selIdx=3, nArgs=1, bcOffset=2.
        check((lifted.valueAt(2).literal & 0xFFFF) == 3, "selIdx=3");
        check(((lifted.valueAt(2).literal >> 16) & 0xFF) == 1, "nArgs=1");
        check(((lifted.valueAt(2).literal >> 24) & 0xFFFFFFFF) == 2,
              "bcOffset=2");

        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower send1 succeeds");

        // Set up a stack, receiver, and temp, then verify the bail path.
        uint64_t stack[8] = {0};
        uint64_t temps[1] = { 0xBBBB };
        FakeState state{};
        state.sp       = &stack[0];
        state.receiver = 0xAAAA;
        state.tempBase = temps;
        fn(&state);

        check(state.exitReason == 2, "exit = ExitSend");
        check(stack[0] == 0xAAAA, "rcvr pushed first");
        check(stack[1] == 0xBBBB, "arg pushed second");
        // sp advanced past both pushes.
        check(state.sp == &stack[2], "sp advanced by 16 bytes");
        std::cout << "--- round-trip send1: exit=" << state.exitReason
                  << " sp-advanced stack={0x" << std::hex << stack[0]
                  << ", 0x" << stack[1] << std::dec << "}\n";
    }

    // Round-trip 8: ^ literal[2] — PushLitConst 2, ReturnTop.
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushLitConstBase + 2),
            SistaV1::ReturnTop,
        };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ^ lit[2] succeeds");

        uint64_t literals[4] = { 0x1100, 0x2200, 0xC0FFEE, 0x4400 };
        FakeState state{};
        state.literals = literals;
        fn(&state);
        check(state.returnValue == 0xC0FFEEULL, "returnValue = literals[2]");
        std::cout << "--- round-trip ^ lit[2]: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    std::cout << "PASS\n";
    return 0;
}
