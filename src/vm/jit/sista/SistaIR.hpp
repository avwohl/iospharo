/*
 * SistaIR.hpp - Method-level SSA-lite IR for the Sista optimizing JIT
 *
 * Phase 2 of docs/sista-inlining-plan.md.  See
 * docs/sista-phase0-decisions.md decision 1 for the rationale.
 *
 * SHAPE
 *   A compiled method is a set of basic blocks.  Each basic block holds
 *   an ordered list of values.  Each value has one definition (SSA)
 *   and zero or more uses.  Control flow between blocks is explicit via
 *   successor / predecessor edges.
 *
 *   Values are numbered globally across the method.  Phi nodes at
 *   multi-predecessor blocks represent merges; single-predecessor
 *   blocks use the predecessor's values directly (no implicit phi).
 *
 * WHY BESPOKE
 *   asmjit's MI IR is C-flavored — it doesn't know about Smalltalk
 *   contexts, sends, or block closures.  MIR was tried and removed
 *   because its spill-liveness-across-calls model lost oops to GC.
 *   Bespoke IR lets us make Oop liveness part of the type system
 *   from day one.
 *
 * SCOPE
 *   This header defines the IR shape only.  Consumers:
 *     - SistaBuilder (TODO): lift bytecode stream into IR.
 *     - SistaLowering (TODO): emit asmjit from IR.
 *     - SistaPrinter (in SistaIR.cpp): textual dump for debugging.
 *     - SistaInliner (TODO, Phase 4): splice callee IR into caller IR.
 *
 * Not yet wired into the runtime.  Compiles standalone; runtime
 * integration happens in Phase 4 once inlining actually works.
 */
#ifndef PHARO_SISTA_IR_HPP
#define PHARO_SISTA_IR_HPP

#include "../../Oop.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace pharo {
namespace sista {

// ===== Operations =====
//
// Every IR value has an op.  Op determines the number and kind of
// operands and the type of the produced value (or void for effects).
enum class Op : uint8_t {
    // --- Constants and loads ---
    kConstantOop,         // operand: 64-bit raw Oop bits                 -> Oop
    kConstantInt,         // operand: 64-bit raw int                      -> Int
    kLoadReceiver,        // 0 operands                                   -> Oop
    kLoadArg,             // operand: arg index                           -> Oop
    kLoadTemp,            // operand: temp index                          -> Oop
    kStoreTemp,           // operands: temp index, value                  -> Void
    kLoadInstVar,         // operands: receiver, instVar index            -> Oop
    kStoreInstVar,        // operands: receiver, instVar index, value     -> Void
    kLoadLiteral,         // operand: literal index                       -> Oop

    // --- Control flow ---
    // Terminators end a block.  They carry successor block IDs.
    kBranch,              // operand: target block id                     -> Void (terminator)
    kBranchIfTrue,        // operands: cond value; succ[0] = true-branch, succ[1] = false -> Void (terminator)
    kBranchIfFalse,       // same but inverted                            -> Void (terminator)
    kReturn,              // operand: value                               -> Void (terminator)
    kNonLocalReturn,      // operand: value, home-frame-depth             -> Void (terminator)

    // --- Sends ---
    kSendUnspeculated,    // operands: selector-literal-idx, nArgs, rcvr, args...
                          // Fall-through to Tier 1 IC probe.              -> Oop
    kGuardClass,          // operands: receiver, expected-class-key, deopt-target
                          // If receiver.class != expected, call deopt.   -> Oop (same as rcvr, typed as inlined class)
    kInlineSend,          // operands: guard-id, callee-method, args...
                          // Inlined callee body follows via nested blocks. -> Oop (callee return)

    // --- Inline primitives ---
    // Narrow-fastpath arith/compare ops that don't need a full send.
    // Typed SmallInt input, SmallInt output; overflow bails via Guard.
    kPrimAddInt,          // operands: a, b                                -> Int
    kPrimSubInt,          // operands: a, b                                -> Int
    kPrimMulInt,          // operands: a, b                                -> Int
    kPrimLtInt,           // operands: a, b                                -> Bool
    kPrimLeInt,           // operands: a, b                                -> Bool
    kPrimGtInt,           // operands: a, b                                -> Bool
    kPrimEqInt,           // operands: a, b                                -> Bool
    kPrimTagCheckInt,     // operand: value; guards SmallInt, deopts else  -> Int

    // --- Blocks ---
    kBlockCreate,         // operands: compiled-block, outer-context, copied-values...
                          // Creates a FullBlockClosure.                    -> Oop
    kBlockValue,          // operands: block, args...  (not inlined)        -> Oop

    // --- Deopt support ---
    // A FrameState value records the interpreter's view of the call
    // stack at this program point: which method, which bytecode, which
    // temps, which stack slots.  Emitted at every potential deopt site
    // and consumed by the deopt-reconstruction runtime.
    kFrameState,          // operands: method oop, bc offset, temps..., stack...  -> FrameState
};

// ===== Value types =====
//
// The type system carries two things the code generator needs:
// (a) whether a value is an Oop (needs GC rooting if live across a
//     potential GC point), and
// (b) the sub-kind for primop fast-paths (e.g. "I know this is a
//     SmallInt so no tag check needed").
enum class Type : uint8_t {
    kVoid,        // no produced value
    kOop,         // any Smalltalk object pointer — GC root
    kOopSmallInt, // Oop known to be a SmallInteger (tag 1)
    kOopChar,     // Oop known to be a Character (tag 3)
    kOopBool,     // Oop known to be true or false
    kInt,         // raw 64-bit integer (NOT a GC root)
    kBool,        // raw 1-bit boolean
    kFrameState,  // synthetic — deopt descriptor, not materialized
};

// ===== Value =====
//
// One IR value with op + operands.  Operands are stored by their
// value id (index into SistaMethod::values).  Terminators have their
// successors stored in the containing SistaBlock.
struct Value {
    uint32_t id;                       // Unique within the method
    Op       op;
    Type     type;
    uint16_t block;                    // Containing block id (index into SistaMethod::blocks)
    // operands by value id.  Variable-length.  Size depends on op.
    std::vector<uint32_t> operands;
    // Optional literal payload: constant bits (kConstantOop, kConstantInt),
    // literal index (kLoadLiteral, kLoadTemp, kStoreTemp, kLoadArg, kLoadInstVar),
    // block id (kBranch), etc.  Interpretation per-op.
    uint64_t literal = 0;
};

// ===== Basic block =====
struct Block {
    uint32_t id;
    std::vector<uint32_t> values;          // value ids in order
    std::vector<uint32_t> successors;      // block ids
    std::vector<uint32_t> predecessors;    // block ids
    // Which source bytecode this block starts at — for deopt framepoint
    // reconstruction.  -1 if synthetic (e.g. deopt landing pad).
    int32_t sourceBytecodeOffset = -1;
};

// ===== Method (IR-level) =====
//
// One compiled method's IR.  Constructed by SistaBuilder, consumed by
// SistaLowering (and optionally transformed by SistaInliner / peephole
// passes in between).
struct Method {
    Oop compiledMethodOop = Oop::nil(); // Original CompiledMethod
    std::vector<Value> values;           // All values, indexed by id
    std::vector<Block> blocks;           // All blocks, indexed by id
    std::vector<Oop>   literals;         // Cached literal table (GC-tracked)
    uint32_t numArgs  = 0;
    uint32_t numTemps = 0;               // Excludes args; just the |...| temps
    uint32_t entryBlock = 0;             // Index into blocks

    // ===== Building helpers =====
    // Append a new block; returns its id.  Predecessor/successor links
    // are set up by addEdge().
    uint32_t newBlock(int32_t sourceBc = -1);

    // Append a new value at the end of the given block; returns value id.
    uint32_t newValue(uint32_t blockId, Op op, Type type,
                       std::vector<uint32_t> operands = {},
                       uint64_t literal = 0);

    // Wire an edge between two blocks (updates both successors and
    // predecessors).
    void addEdge(uint32_t from, uint32_t to);

    // ===== Introspection =====
    const Value& valueAt(uint32_t id) const { return values[id]; }
    const Block& blockAt(uint32_t id) const { return blocks[id]; }

    // Textual dump for debugging.  Defined in SistaIR.cpp.
    std::string toString() const;
};

// ===== Op metadata =====
//
// Central place to answer "what kind of op is this" so passes don't
// re-implement the classification.
namespace OpInfo {
    bool isTerminator(Op op);        // Ends its containing block
    bool producesOop(Op op);         // Result is a GC root
    bool mayCallBack(Op op);         // Might transfer to interpreter (deopt, send, prim)
    bool readsStack(Op op);          // Reads interpreter stack
    bool writesStack(Op op);         // Writes interpreter stack
    const char* name(Op op);         // For textual dump
}

}  // namespace sista
}  // namespace pharo

#endif  // PHARO_SISTA_IR_HPP
