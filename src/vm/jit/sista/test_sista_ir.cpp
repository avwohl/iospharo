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

    // Phase 3 framepoint capture: every kSendUnspeculated emission
    // should produce a Framepoint entry recording (valueId, bcOffset,
    // stack snapshot).  Verifies infrastructure for monomorphic inlining
    // deopt landing pads (Phase 4).  Run early (before pre-existing
    // setter-lower fail) so the framepoint checks always execute.
    {
        // Send0 #foo: PushReceiver, Send0(sel-0).
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,                  // 0x4C — bc offset 0
            (uint8_t)(SistaV1::Send0Base + 0),      // 0x80 — bc offset 1
            SistaV1::ReturnTop,                     // unreachable
        };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);
        check(lifted.framepoints.size() == 1,
              "1 framepoint for 1 Send0");
        const auto& fp = lifted.framepoints[0];
        check(fp.bcOffset == 1, "framepoint bcOffset = Send0 ip (1)");
        check(fp.stackValueIds.size() == 1,
              "framepoint stack snapshot has 1 entry (the receiver)");
        check(lifted.valueAt(fp.valueId).op == Op::kSendUnspeculated,
              "framepoint valueId points at kSendUnspeculated");
        std::cout << "--- framepoint Send0: bc=" << (int)fp.bcOffset
                  << " stack=" << fp.stackValueIds.size()
                  << " val=" << fp.valueId << "\n";
    }
    {
        // Send1 #bar:: PushReceiver, PushTemp 0, Send1(sel-0).
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,                  // 0x4C — bc 0
            (uint8_t)(SistaV1::PushTempBase + 0),   // 0x40 — bc 1
            (uint8_t)(SistaV1::Send1Base + 0),       // 0x90 — bc 2
            SistaV1::ReturnTop,                      // unreachable
        };
        Builder::buildFromBytes(bc, sizeof(bc), 1, 0, lifted);
        check(lifted.framepoints.size() == 1,
              "1 framepoint for 1 Send1");
        const auto& fp = lifted.framepoints[0];
        check(fp.bcOffset == 2, "framepoint bcOffset = Send1 ip (2)");
        check(fp.stackValueIds.size() == 2,
              "framepoint stack snapshot = receiver + 1 arg");
        check(lifted.valueAt(fp.valueId).op == Op::kSendUnspeculated,
              "framepoint valueId points at kSendUnspeculated");
        std::cout << "--- framepoint Send1: bc=" << (int)fp.bcOffset
                  << " stack=" << fp.stackValueIds.size() << "\n";
    }
    // Yourself: no Send → no framepoint
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,
            SistaV1::ReturnTop,
        };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);
        check(lifted.framepoints.empty(),
              "no framepoints for send-free method");
    }

    // kPrimIdentityEq metadata: produces oop, may not call back
    check(OpInfo::producesOop(Op::kPrimIdentityEq),
          "kPrimIdentityEq produces an Oop");
    check(!OpInfo::mayCallBack(Op::kPrimIdentityEq),
          "kPrimIdentityEq has no deopt path");
    check(std::string(OpInfo::name(Op::kPrimIdentityEq))
          == "prim_identity_eq",
          "kPrimIdentityEq names correctly");

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

    // Unsupported bytecode bails cleanly.  CallPrimitive in a
    // non-method-start position is still unsupported (we only skip
    // at offset 0, per the Smalltalk convention that primitive
    // declarations appear only at method entry).
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushOne,                  // 0: ip != 0 at 0xF8
            SistaV1::CallPrimitive, 0, 0,      // 1,2,3
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc),
                                                 0, 0, lifted, &failed);
        check(r == LiftResult::kUnsupportedBytecode, "bail on unsupported");
        check(failed == 1, "fails at offset 1 (CallPrimitive)");
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

    // Phase 4 setter-shape tests: lift-only — verify the IR shapes the
    // inliner pattern-matches against (tryInlineConstReturn).  If the
    // lifter ever changes shape, this test fires before the inliner
    // silently stops firing.  Placed before the round-trip setter test
    // so a lowering regression there doesn't mask shape regressions.
    {
        // 5-value implicit-returnSelf setter:
        //   pushTemp 0; popStoreRcv 1; returnReceiver
        Method m;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushTempBase + 0),
            (uint8_t)(SistaV1::PopStoreRecvBase + 1),
            SistaV1::ReturnReceiver,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 1, 1, m);
        check(r == LiftResult::kOk, "5-value setter lifts");
        check(m.values.size() == 5, "5-value setter has 5 IR ops");
        check(m.values[0].op == Op::kLoadTemp,    "v0 = kLoadTemp");
        check(m.values[1].op == Op::kLoadReceiver, "v1 = kLoadReceiver");
        check(m.values[2].op == Op::kStoreInstVar, "v2 = kStoreInstVar");
        check(m.values[3].op == Op::kLoadReceiver, "v3 = kLoadReceiver");
        check(m.values[4].op == Op::kReturn,       "v4 = kReturn");
        check(m.values[2].operands.size() == 2 &&
              m.values[2].operands[0] == m.values[1].id &&
              m.values[2].operands[1] == m.values[0].id,
              "kStoreInstVar reads recv + tempVal");
        check(m.values[4].operands.size() == 1 &&
              m.values[4].operands[0] == m.values[3].id,
              "kReturn returns the receiver");
        std::cout << "--- pattern: 5-value implicit-returnSelf setter\n";
    }
    {
        // 4-value return-value setter `^ foo := x`:
        //   pushTemp 0; ExtStoreRecv 1 (no-pop); returnTop
        Method m;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushTempBase + 0),
            SistaV1::ExtStoreRecv, 0x01,
            SistaV1::ReturnTop,
        };
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 1, 1, m);
        check(r == LiftResult::kOk, "4-value setter lifts");
        check(m.values.size() == 4, "4-value setter has 4 IR ops");
        check(m.values[0].op == Op::kLoadTemp,    "v0 = kLoadTemp");
        check(m.values[1].op == Op::kLoadReceiver, "v1 = kLoadReceiver");
        check(m.values[2].op == Op::kStoreInstVar, "v2 = kStoreInstVar");
        check(m.values[3].op == Op::kReturn,       "v3 = kReturn");
        check(m.values[3].operands.size() == 1 &&
              m.values[3].operands[0] == m.values[0].id,
              "kReturn returns the assigned value (kLoadTemp)");
        std::cout << "--- pattern: 4-value return-value setter\n";
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

    // Round-trip 7.E: Phi at join — ^ (cond ifTrue:[1] ifFalse:[0]) + 1.
    // Pattern:
    //   0: PushTrue        (condition)
    //   1: JumpIfFalse +2  (0xC1 → target bc=4)
    //   2: PushOne         (true branch: push SmallInt 1)
    //   3: Jump +2         (0xB2 → target bc=6, skipping false branch)
    //   4: PushZero        (false branch: push SmallInt 0)
    //                       (fall-through to bc=5)
    //   5: ???
    //
    // Let me simplify.  Use explicit unconditional jump to join:
    //   0: PushTrue
    //   1: JumpIfFalse +2  → target bc=4
    //   2: PushOne
    //   3: Jump +1         → target bc=5 (skip false branch)
    //   4: PushZero
    //   5: PushOne
    //   6: Add
    //   7: ReturnTop
    //
    // shortJumpTarget(1, 0xC1) = 1 + 1 + 2 = 4.   (JumpIfFalse +2)
    // shortJumpTarget(3, 0xB0) = 3 + 0 + 2 = 5.   (Jump +0 → 5? no)
    //   Short uncond jump N has offset (N&7)+1 + 1 = N+2.  For target=5
    //   from ip=3, need offset=2 → opcode = 0xB0 + 0 = 0xB0.  (0xB0
    //   means +1 per the SistaV1 offset formula?  Double-check via
    //   shortJumpTarget: shortJumpTarget(3, 0xB0) = 3 + 0 + 2 = 5. ✓)
    //
    // Expected:
    //   taken (true) path: 1 + 1 = 2  → SmallInt bits 0x11
    //   untaken (false) path: 0 + 1 = 1 → SmallInt bits 0x9
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushTrue,                // 0
            0xC1,                              // 1: JumpIfFalse +2 → 4
            SistaV1::PushOne,                 // 2
            0xB0,                              // 3: Jump → 5
            SistaV1::PushZero,                // 4: (false branch)
            SistaV1::PushOne,                 // 5: (join point)
            (uint8_t)(SistaV1::ArithBase + 0),// 6: +
            SistaV1::ReturnTop,               // 7
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "phi lifts");
        std::cout << "\n--- lifted phi method ---\n" << lifted.toString();

        // Verify a phi was created at the join block.  The join block
        // has 2 predecessors (true-branch and false-branch).
        bool sawPhi = false;
        for (const Value& v : lifted.values) {
            if (v.op == Op::kPhi) {
                sawPhi = true;
                check(v.operands.size() == 2,
                      "phi has 2 incoming operands");
                break;
            }
        }
        check(sawPhi, "saw at least one phi");

        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower phi succeeds");

        // cond=true path: phi = 1, + 1 = 2 → 0x11.
        FakeStateWithBools state{};
        state.trueOop  = 0x1111;
        state.falseOop = 0x2222;
        fn(&state);
        check(state.returnValue == 0x11ULL, "true path: 1+1 = SmallInt 2");

        // cond=false path: swap to PushFalse and re-lower.
        {
            Method lifted2;
            const uint8_t bc2[] = {
                SistaV1::PushFalse,
                0xC1,
                SistaV1::PushOne,
                0xB0,
                SistaV1::PushZero,
                SistaV1::PushOne,
                (uint8_t)(SistaV1::ArithBase + 0),
                SistaV1::ReturnTop,
            };
            Builder::buildFromBytes(bc2, sizeof(bc2), 0, 0, lifted2);
            Lowering::CompiledFn fn2 = lowering.lower(lifted2);
            check(fn2 != nullptr, "lower false-path phi succeeds");
            FakeStateWithBools s{};
            s.trueOop  = 0x1111;
            s.falseOop = 0x2222;
            fn2(&s);
            check(s.returnValue == 0x9ULL,
                  "false path: 0+1 = SmallInt 1 (bits 0x9)");
        }
        std::cout << "--- round-trip phi merge: taken=0x11 untaken=0x9 ok\n";
    }

    // Round-trip 7.F: ExtJump forward — medium-distance unconditional.
    // Skip a dead block via ExtJump +1.  The dead block ends in
    // ReturnFalse (terminator, outgoing stack empty) so the jump
    // target sees no predecessor-stack mismatch.
    //
    // ExtJump layout: [0xED, offsetByte]; offset = byte + extB*256.
    // With extB=0 and offsetByte=1, target = (0+2) + 1 = 3.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::ExtJump, 0x01,      // 0,1: jump to bc=3
            SistaV1::ReturnFalse,        // 2:   dead terminator
            SistaV1::PushOne,            // 3:
            SistaV1::ReturnTop,          // 4:
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ExtJump +1 lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower ExtJump +1");
        FakeStateWithBools state{};
        state.trueOop  = 0xAAAA;
        state.falseOop = 0xBBBB;
        fn(&state);
        check(state.returnValue == 9ULL, "ExtJump skips dead block; ^1");
        std::cout << "--- round-trip ExtJump +1: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.G: ExtJumpFalse — medium-distance conditional.
    // Pattern: PushFalse, ExtJumpFalse +2, PushOne, ReturnTop,
    //          PushZero, ReturnTop
    // False → take (skip PushOne), push 0, return 0 → SmallInt 0 bits = 1.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushFalse,          // 0
            SistaV1::ExtJumpFalse, 0x02, // 1,2: if false, jump to bc=5
            SistaV1::PushOne,            // 3
            SistaV1::ReturnTop,          // 4
            SistaV1::PushZero,           // 5 (taken branch)
            SistaV1::ReturnTop,          // 6
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ExtJumpFalse lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower ExtJumpFalse");
        FakeStateWithBools state{};
        state.trueOop  = 0xAAAA;
        state.falseOop = 0xBBBB;
        fn(&state);
        check(state.returnValue == 1ULL,
              "ExtJumpFalse taken → SmallInt 0 bits = 1");
        std::cout << "--- round-trip ExtJumpFalse: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.H.1: Backward ExtJump (loop) with a forward entry.
    // The entry block jumps FORWARD into the loop body, and the loop
    // body has a backward jump to itself.  This makes the loop header
    // reachable (from the entry block) so orphan-skip doesn't elide it.
    //
    // Structure:
    //   0,1: ExtJump +0 → target = 2 (forward enter to loop)
    //   2,3: ExtendB 0xFF  (extB = -1)
    //   4,5: ExtJump 0xFC  (offset = 252-256 = -4 → target = 6-4 = 2)
    // The test doesn't execute (infinite loop); it only verifies that
    // the lifter records a backward edge from block-1 to itself.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::ExtJump, 0x00,         // 0,1 → jump to bc=2
            SistaV1::ExtendB, 0xFF,         // 2,3 (loop header: extB = -1)
            SistaV1::ExtJump, 0xFC,         // 4,5 (backward -4 → bc=2)
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "backward loop lifts");
        // Loop header block should have itself as one of its
        // predecessors (the backward edge) plus the entry forward edge.
        bool sawSelfLoop = false;
        for (const Block& b : lifted.blocks) {
            for (uint32_t pred : b.predecessors) {
                if (pred == b.id) { sawSelfLoop = true; break; }
            }
            if (sawSelfLoop) break;
        }
        check(sawSelfLoop, "backward edge creates self-loop predecessor");

        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower backward loop succeeds");
        std::cout << "--- round-trip backward-ExtJump loop: ok (not executed)\n";
    }

    // Round-trip 7.H: ExtendB prefix gives ExtJump 256-byte reach.
    // Use ExtendB 0, ExtJump +1 — equivalent to plain ExtJump +1 but
    // exercises the prefix code path.  Dead block terminates to keep
    // predecessor stacks aligned.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::ExtendB, 0x00,      // 0,1: extB = 0 (no-op high byte)
            SistaV1::ExtJump, 0x01,      // 2,3: jump to bc=5
            SistaV1::ReturnFalse,        // 4:   dead terminator
            SistaV1::PushOne,            // 5
            SistaV1::ReturnTop,          // 6
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ExtendB + ExtJump lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower ExtendB + ExtJump");
        FakeState state{};
        fn(&state);
        check(state.returnValue == 9ULL, "ExtendB 0 + ExtJump +3 → ^1");
        std::cout << "--- round-trip ExtendB+ExtJump: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.I: ExtPushTemp — temp index beyond short range.
    // PushTemp (short) covers 0..11; ExtPushTemp reaches 0..255.
    // Test reads temp[20] which is beyond the short range.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::ExtPushTemp, 20,     // 0,1: push temp[20]
            SistaV1::ReturnTop,           // 2
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 21,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ExtPushTemp 20 lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower ExtPushTemp 20");
        uint64_t temps[21];
        for (int i = 0; i < 21; i++) temps[i] = 0x1000 + i;
        FakeState state{};
        state.tempBase = temps;
        fn(&state);
        check(state.returnValue == 0x1014ULL,
              "returnValue = temp[20] = 0x1014");
        std::cout << "--- round-trip ExtPushTemp 20: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.J: ExtendA + ExtPushLitConst — lit index > 31.
    // ExtendA 1 + ExtPushLitConst 5 → lit index = (1 << 8) | 5 = 261.
    // But test using a smaller index (no extA) to avoid oversized
    // literal arrays — lit index = 33 via ExtPushLitConst 33 (extA=0).
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::ExtPushLitConst, 33, // 0,1: push lit[33]
            SistaV1::ReturnTop,           // 2
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ExtPushLitConst 33 lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower ExtPushLitConst 33");
        uint64_t literals[40];
        for (int i = 0; i < 40; i++) literals[i] = 0x2000 + i;
        FakeState state{};
        state.literals = literals;
        fn(&state);
        check(state.returnValue == 0x2021ULL,
              "returnValue = literals[33] = 0x2021");
        std::cout << "--- round-trip ExtPushLitConst 33: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.K: ExtPopStoreTemp 15, ExtPushTemp 15.
    // Stores and re-reads temp index 15 (out of short range).
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushOne,                      // 0
            SistaV1::ExtPopStoreTemp, 15,          // 1,2
            SistaV1::ExtPushTemp,     15,          // 3,4
            SistaV1::ReturnTop,                    // 5
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 16,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ext store/push temp 15 lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed);
        check(fn != nullptr, "lower ext store/push temp 15");
        uint64_t temps[16] = {0};
        FakeState state{};
        state.tempBase = temps;
        fn(&state);
        check(state.returnValue == 9ULL,
              "stored PushOne, re-read SmallInt 1 (bits 9)");
        check(temps[15] == 9ULL, "temp[15] holds stored SmallInt 1");
        std::cout << "--- round-trip ext store/push temp 15: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.L: PushInteger — inline integer literal.
    // Method: ^ 42.  PushInteger 42 (2-byte), ReturnTop.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushInteger, 42,
            SistaV1::ReturnTop,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "PushInteger 42 lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower PushInteger 42");
        FakeState state{};
        fn(&state);
        // SmallInt 42 bits = (42 << 3) | 1 = 337 = 0x151.
        check(state.returnValue == 0x151ULL, "^42 → 0x151");
        std::cout << "--- round-trip PushInteger 42: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.M: PushInteger with ExtendB prefix for negative.
    // ExtendB -1 + PushInteger 0xFF → value = 0xFF + (-1)*256 = -1.
    // SmallInt -1 bits = (0xFFFFFFFFFFFFFFFF << 3) | 1 = 0xFFFFFFFFFFFFFFF9.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::ExtendB, 0xFF,
            SistaV1::PushInteger, 0xFF,
            SistaV1::ReturnTop,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ExtB -1 + PushInteger -1 lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ExtB -1 + PushInteger -1");
        FakeState state{};
        fn(&state);
        uint64_t expected = ((uint64_t)(int64_t)-1 << 3) | 1;
        check(state.returnValue == expected,
              "^-1 → SmallInt -1 tagged bits");
        std::cout << "--- round-trip PushInteger -1: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.N: PushCharacter 'A' (codepoint 65).
    // Character Oop bits = (65 << 3) | 3 = 0x20B.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushCharacter, 65,
            SistaV1::ReturnTop,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "PushCharacter 65 lifts");
        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower PushCharacter 65");
        FakeState state{};
        fn(&state);
        check(state.returnValue == 0x20BULL, "^$A → 0x20B");
        std::cout << "--- round-trip PushCharacter 'A': returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Round-trip 7.O: ExtSend with 3 args.  desc = (sel<<3) | nArgs:
    // sel=2, nArgs=3 → desc = 0x13.  rcvr + 3 args pushed first.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,                         // 0
            (uint8_t)(SistaV1::PushTempBase + 0),          // 1 (arg 0)
            (uint8_t)(SistaV1::PushTempBase + 1),          // 2 (arg 1)
            (uint8_t)(SistaV1::PushTempBase + 2),          // 3 (arg 2)
            SistaV1::ExtSend, 0x13,                        // 4,5 → sel=2, nArgs=3
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 3, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "ExtSend 3-arg lifts");
        // Check the emitted op.
        bool sawSend = false;
        for (const Value& v : lifted.values) {
            if (v.op == Op::kSendUnspeculated) {
                sawSend = true;
                check(v.operands.size() == 4, "rcvr + 3 args");
                check((v.literal & 0xFFFF) == 2, "selIdx=2");
                check(((v.literal >> 16) & 0xFF) == 3, "nArgs=3");
                break;
            }
        }
        check(sawSend, "emitted kSendUnspeculated");

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower ExtSend 3-arg");

        uint64_t stack[8] = {0};
        uint64_t temps[3] = { 0xAAA1, 0xAAA2, 0xAAA3 };
        FakeState state{};
        state.sp       = &stack[0];
        state.receiver = 0xBEEF;
        state.tempBase = temps;
        fn(&state);
        check(state.exitReason == 2, "ExtSend bails with ExitSend");
        check(stack[0] == 0xBEEF, "rcvr first");
        check(stack[1] == 0xAAA1, "arg0 second");
        check(stack[2] == 0xAAA2, "arg1 third");
        check(stack[3] == 0xAAA3, "arg2 fourth");
        check(state.sp == &stack[4], "sp advanced by 32 bytes");
        std::cout << "--- round-trip ExtSend 3-arg: exit=" << state.exitReason
                  << " stack pushed 4 oops\n";
    }

    // Round-trip 7.P: PushLitVar — literal is an Association, push
    // its .value slot (slot 1 at +16 bytes from Association Oop).
    //
    // Method: ^ MyGlobal  lifts as literals[0]=Association, then
    // fetches Association.value.  Single bytecode: PushLitVar 0.
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushLitVarBase + 0),  // 0x10
            SistaV1::ReturnTop,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "PushLitVar 0 lifts");
        // Two ops: kLoadLiteral + kLoadInstVar(slot=1).
        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower PushLitVar 0");

        // Fake Association at literals[0]: header at +0, slot 0 (key)
        // at +8, slot 1 (value) at +16.
        uint64_t assoc[3] = { 0xDEADDEAD, 0xA11A11, 0xFEEDFACE };
        uint64_t literals[1] = {
            reinterpret_cast<uint64_t>(&assoc[0]),
        };
        FakeState state{};
        state.literals = literals;
        fn(&state);
        check(state.returnValue == 0xFEEDFACEULL,
              "returnValue = association.value");
        std::cout << "--- round-trip PushLitVar 0: returnValue=0x"
                  << std::hex << state.returnValue << std::dec << "\n";
    }

    // Diagnostic: replay a bytecode pattern that malforms over real
    // images — expose the exact failure path for debugging.
    {
        Method lifted;
        // From a Pharo 13 CompiledMethod seen in the survey.  First
        // byte is PushLitVar, then two sends, then ReturnTop.  The
        // remaining bytes are unreachable from compiled code (only
        // the interpreter reaches them via the send bail).
        const uint8_t bc[] = {
            0x10, 0x81, 0x82, 0x5C,        // push litvar, send0, send0, return
            0x00, 0x02, 0x26, 0x1D, 0xB0,  // short jump target should exist
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x33, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
            0x28, 0xB0, 0x05, 0x20, 0x01, 0x00, 0x00, 0x00,
            0x65, 0x0C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04,
            0xC0, 0x81, 0xE8, 0x1F, 0x01, 0x00, 0x00, 0x00,
            0x50, 0xA2, 0x05, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00,
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        // Block 0 ends in a send-bail.  All other blocks are
        // orphan (no forward predecessor — block 0's terminator is
        // a send, not a branch).  Orphan-skip should elide them
        // and lifting should succeed.
        if (r != LiftResult::kOk) {
            std::fprintf(stderr, "MALFORM DIAG: result=%d failedAt=%u\n",
                         (int)r, failed);
        }
        check(r == LiftResult::kOk, "malform sample lifts via orphan-skip");
        std::cout << "--- diag: malform sample ok (orphan-skip works)\n";
    }

    // Round-trip 7.Q: bytecodeBase wiring — send bail uses absolute
    // state.ip (bytecodeBase + bcOffset), not just the offset.  This
    // is what runtime integration needs.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,          // 0
            (uint8_t)(SistaV1::Send0Base),  // 1: Send0 sel=0
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "send0 lifts");

        // Pretend `bc` lives at address 0x1000 (fake, not actual).
        const uint8_t* fakeBase = reinterpret_cast<const uint8_t*>(0x1000);
        Lowering::CompiledFn fn = lowering.lower(lifted, &failed, fakeBase);
        check(fn != nullptr, "lower with bytecodeBase succeeds");

        // FakeState with an ip slot at offset 48.
        struct FakeStateWithIp {
            void*    sp;
            uint64_t receiver;
            void*    literals;
            void*    tempBase;
            uint8_t  padA[16];
            uint8_t* ip;              // offset 48
            uint8_t  padB[20];
            int      exitReason;      // 76
            uint64_t returnValue;
            uint8_t  padC[256];
        };
        static_assert(offsetof(FakeStateWithIp, ip) == 48, "ip offset");

        uint64_t stack[4] = {0};
        FakeStateWithIp state{};
        state.sp       = &stack[0];
        state.receiver = 0xABCD;
        fn(&state);

        check(state.exitReason == 2, "exit = ExitSend");
        // state.ip should now be fakeBase + bcOffset (=1, the send byte).
        check(state.ip == (fakeBase + 1),
              "state.ip = bytecodeBase + send bcOffset");
        std::cout << "--- round-trip bytecodeBase: state.ip=0x"
                  << std::hex << (uint64_t)state.ip << std::dec << "\n";
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

    // Round-trip 9: Send1 bail with PushRecvVar operands.
    // Reproduces the shape seen in the real image #reset method
    // (bytecodes `02 01 90 …`): two instVar pushes feeding a Send1.
    // At bail the IR stack [ivar[2], ivar[1]] should be pushed to
    // state.sp in receiver-first order.
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushRecvVarBase + 2),   // 0x02
            (uint8_t)(SistaV1::PushRecvVarBase + 1),   // 0x01
            (uint8_t)(SistaV1::Send1Base + 0),          // 0x90
            SistaV1::ReturnTop,                         // unreachable
        };
        uint32_t failed = UINT32_MAX;
        LiftResult r = Builder::buildFromBytes(bc, sizeof(bc), 0, 0,
                                                 lifted, &failed);
        check(r == LiftResult::kOk, "Send1 w/ ivars lifts");

        // Confirm lift shape: two kLoadInstVar + one kSendUnspeculated.
        size_t ivarLoads = 0, sendCount = 0;
        for (const Value& v : lifted.values) {
            if (v.op == Op::kLoadInstVar) ivarLoads++;
            if (v.op == Op::kSendUnspeculated) sendCount++;
        }
        check(ivarLoads == 2, "two kLoadInstVar");
        check(sendCount == 1, "one kSendUnspeculated");

        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower Send1 w/ ivars");

        // Fake receiver object: slot 0 at +8, slot 1 at +16, slot 2 at +24.
        uint64_t recvObj[4] = {
            0xDEADBEEFULL,   // header
            0x1111,           // instVar[0]
            0x2222,           // instVar[1] — should appear as arg on stack
            0x3333,           // instVar[2] — should appear as rcvr on stack
        };
        uint64_t stack[4] = {0};
        FakeState state{};
        state.sp       = &stack[0];
        state.receiver = reinterpret_cast<uint64_t>(&recvObj[0]);
        fn(&state);
        check(state.exitReason == 2, "Send1 bails with ExitSend");
        check(stack[0] == 0x3333ULL,
              "push[0] = receiver's instVar[2]");
        check(stack[1] == 0x2222ULL,
              "push[1] = receiver's instVar[1]");
        check(state.sp == &stack[2],
              "sp advanced by 2 slots (rcvr + 1 arg)");
        std::cout << "--- round-trip Send1 w/ PushRecvVar: "
                     "stack[0]=0x" << std::hex << stack[0]
                  << " stack[1]=0x" << stack[1] << std::dec << "\n";
    }

    // Round-trip 9b: Same Send1 pattern, but with SmallInt 0 values
    // in both instVars — matches the real-image #reset case where
    // push[0]==push[1]==0x1 raised concern.  Confirms that bit
    // pattern is just what the receiver actually holds, not a bug.
    {
        Method lifted;
        const uint8_t bc[] = {
            (uint8_t)(SistaV1::PushRecvVarBase + 2),
            (uint8_t)(SistaV1::PushRecvVarBase + 1),
            (uint8_t)(SistaV1::Send1Base + 0),
            SistaV1::ReturnTop,
        };
        Builder::buildFromBytes(bc, sizeof(bc), 0, 0, lifted);
        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower Send1 w/ SmallInt-0 ivars");

        uint64_t recvObj[4] = {
            0xDEADBEEFULL,   // header
            0x0,              // instVar[0]
            0x1,              // instVar[1] = SmallInt 0
            0x1,              // instVar[2] = SmallInt 0
        };
        uint64_t stack[4] = {0};
        FakeState state{};
        state.sp       = &stack[0];
        state.receiver = reinterpret_cast<uint64_t>(&recvObj[0]);
        fn(&state);
        check(state.exitReason == 2, "Send1 bails with ExitSend");
        check(stack[0] == 0x1ULL, "push[0] = SmallInt 0");
        check(stack[1] == 0x1ULL, "push[1] = SmallInt 0");
        std::cout << "--- round-trip Send1 w/ SmallInt-0 ivars: "
                     "push[0,1]=SmallInt 0 — matches real VM observation\n";
    }

    // Round-trip 9c: Send1 with PushReceiver + PushTemp — the
    // textbook shape for `^ self foo: anArg`.  Most image methods
    // that Send1 use this pattern.
    {
        Method lifted;
        const uint8_t bc[] = {
            SistaV1::PushReceiver,                     // 0x4C: self
            (uint8_t)(SistaV1::PushTempBase + 0),      // 0x40: temp 0 (arg)
            (uint8_t)(SistaV1::Send1Base + 0),          // 0x90: Send1 sel-0
            SistaV1::ReturnTop,                         // unreachable
        };
        Builder::buildFromBytes(bc, sizeof(bc), 1, 0, lifted);
        Lowering::CompiledFn fn = lowering.lower(lifted);
        check(fn != nullptr, "lower Send1 w/ Receiver+Temp");

        uint64_t stack[4] = {0};
        uint64_t temps[4] = { 0xA11A11, 0, 0, 0 };
        FakeState state{};
        state.sp       = &stack[0];
        state.receiver = 0xDEADBEEF;
        state.tempBase = temps;
        fn(&state);
        check(state.exitReason == 2, "Send1 Receiver+Temp ExitSend");
        check(stack[0] == 0xDEADBEEFULL, "push[0] = self");
        check(stack[1] == 0xA11A11ULL,    "push[1] = temp[0]");
        check(state.sp == &stack[2],      "sp += 2 slots");
        std::cout << "--- round-trip Send1 w/ Receiver+Temp: "
                     "stack[0]=0x" << std::hex << stack[0]
                  << " stack[1]=0x" << stack[1] << std::dec << "\n";
    }

    std::cout << "PASS\n";
    return 0;
}
