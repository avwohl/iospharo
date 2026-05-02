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
 *     - SistaBuilder: lifts bytecode into IR (lift + speculation +
 *       Phase 4 monomorphic inliner + B2 splice pre-pass).
 *     - SistaLowering: emits asmjit ARM64 from IR.
 *     - SistaPrinter (in SistaIR.cpp): textual dump for debugging.
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
    kLoadTrueOop,         // 0 operands; reads from JITState               -> OopBool
    kLoadFalseOop,        // 0 operands; reads from JITState               -> OopBool
    kLoadArg,             // literal: arg index                           -> Oop
    kLoadTemp,            // literal: temp index                          -> Oop
    kStoreTemp,           // operand[0] = value; literal: temp index      -> Void
    kLoadTempInVec,       // 0 operands; literal: high 32 = tempIdxOfVec,
                          //                       low  32 = indexInVec     -> Oop
                          // Push (temps[tempIdxOfVec])[indexInVec]: the
                          // closure-captured variable at slot `indexInVec`
                          // of the TempVector held in outer temp `tempIdxOfVec`.
    kStoreTempInVec,      // operand[0] = value; literal layout same as
                          // kLoadTempInVec.                                -> Void
    kAllocArray,          // 0 operands; literal: array size               -> Oop
                          // Allocates a fresh Array(size) initialized to
                          // nil.  Mirrors PushNewArray (0xE7) j=0 path.
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
    kSendUnspeculated,    // operands: rcvr, args...
                          // literal packs the send descriptor:
                          //   bits  0-15: selector literal index
                          //   bits 16-23: nArgs
                          //   bits 24-55: send bytecode offset (for state.ip on bail)
                          // Fall-through to Tier 1 IC probe via ExitSend —
                          // compiled code exits at the send and the
                          // interpreter resumes the bytecode stream.      -> Oop

    kSendCallHelper,      // operands: rcvr, args...
                          // literal: low 32 = selector literal index,
                          //          mid 16 = nArgs,
                          //          high 16 = bcOffset (for NLR-deopt resume).
                          // Lowering invokes jit_rt_sista_call_send via
                          // cc.invoke.  The helper does the send synchronously
                          // and returns the result; compiled code continues
                          // past the send.  On NLR / abnormal exit, the
                          // helper returns 0 and lowering deopts to the
                          // source bcOffset (interpreter handles NLR).
                          // Result type: same as kSendUnspeculated.        -> Oop
    kGuardClass,          // operands: receiver, expected-class-key, deopt-target
                          // If receiver.class != expected, call deopt.   -> Oop (same as rcvr, typed as inlined class)
    kInlineSend,          // operands: guard-id, callee-method, args...
                          // Inlined callee body follows via nested blocks. -> Oop (callee return)

    // --- Inline primitives ---
    // Narrow-fastpath arith/compare on tagged SmallInt operands.
    // Operate DIRECTLY on the Oop bits using tag-preserving math:
    //   `a + b - 1`   gives the correctly-tagged sum when both
    //                 are tag-1 SmallInts.
    //   `a - b + 1`   gives the correctly-tagged difference.
    // Overflow / non-SmallInt checks are the lowerer's responsibility;
    // they bail to the interpreter via the standard send protocol.
    // Phase 2 landed the op bodies but not the safety checks — those
    // ship with Phase 3 deopt infrastructure.
    kPrimAddInt,          // operands: a, b (SmallInt Oops)          -> OopSmallInt
    kPrimSubInt,          // operands: a, b                          -> OopSmallInt
    kPrimMulInt,          // operands: a, b                          -> OopSmallInt
    kPrimLtInt,           // operands: a, b                          -> OopBool
    kPrimLeInt,           // operands: a, b                          -> OopBool
    kPrimGtInt,           // operands: a, b                          -> OopBool
    kPrimGeInt,           // operands: a, b                          -> OopBool
    kPrimEqInt,           // operands: a, b                          -> OopBool
    kPrimNeqInt,          // operands: a, b                          -> OopBool
    kPrimTagCheckInt,     // operand: value; deopts if not SmallInt  -> OopSmallInt
    kPrimIdentityEq,      // operands: a, b (any Oops); cmp identity -> OopBool
                          // Result is trueOop if a == b (raw bits), else falseOop.
                          // Used by Phase 4 inline of #== (universal semantics).
    kPrimIdentityNeq,     // operands: a, b; like kPrimIdentityEq but negated
                          // (trueOop if a != b).  Phase 4 inline of #~~.

    // --- Indexable primitives (for B2 splice) ---
    // Lower to runtime-helper calls via cc.invoke; helpers return 0
    // on receiver-class miss / OOB / non-SmI index — SistaLowering
    // emits a deopt-bail when the result is 0 (so the interpreter
    // re-runs the original send, including basicAt:put: and
    // bounds-error paths).
    kPrimSize,            // operands: receiver                        -> OopSmallInt
                          // Returns receiver basicSize as SmI Oop.
    kPrimAt,              // operands: receiver, index                 -> Oop
                          // Returns receiver basicAt: index, or
                          // signals deopt via 0 result.

    // --- Blocks ---
    kBlockCreate,         // operands: compiled-block, outer-context, copied-values...
                          // Creates a FullBlockClosure.                    -> Oop
    kBlockValue,          // operands: block, args...  (not inlined)        -> Oop

    // --- B2 splice ---
    // Counted at: loop with the block body inlined.  Replaces the
    // sequence `PushFullBlock; SpecialSend(do:)` for cases where the
    // IC says receiver class is Array AND the block sub-lifts to
    // splice-simple IR.  Result on stack: the receiver (do: returns
    // its receiver).
    //
    // operands[0] = receiver (the array)
    // literal     = index into Method::inlinedBlocks for the spliced
    //               block body IR.
    //
    // Lowering emits: i=1; size=basicSize(rcvr); while i<=size do
    //   each := basicAt(rcvr,i); <inlined body with each subbed>;
    //   i := i+1. push rcvr.
    //
    // Bails to interpreter if rcvr is non-indexable (basicSize fails)
    // or if any inlined body op deopts.  Gated behind
    // PHARO_SISTA_DO_SPLICE=1; default off until end-to-end validated.
    kCountedLoopDo,       // operands: receiver  -> Oop  (the receiver)

    // --- B2 splice: inject:into: variant ---
    // Recognized PushFullBlock+Send2(#inject:into:) pattern.  Fully
    // inlines the iteration as a counted at: loop with the
    // accumulator held in a register; block has 2 args (acc, elem)
    // and returns the new acc as its value.
    //
    // operands: receiver, init                 -> Oop  (final acc)
    // literal:  block-IR slot in inlinedBlocks
    //
    // Side-effect-free invariant: the block must be a pure function
    // of (acc, elem); no stores allowed.  Mid-loop deopt rolls back
    // to init (no partial commit), so the interpreter can re-run the
    // entire inject:into: send safely.
    kCountedLoopInjectInto,

    // --- B2 splice: Interval marker ---
    // Emitted by the lifter for `<start> to: <stop>` Send1 sites
    // recognized as part of an Interval-inject pattern.  Holds
    // start+stop as a pseudo-Interval value on the IR stack.  Never
    // lowered directly — the inject:into: intercept reads its
    // operands and replaces it with kCountedLoopIntervalInjectInto.
    //
    // operands: start, stop  -> Oop (pseudo-Interval; never reified)
    kInterval,

    // --- B2 splice: (start to: stop) inject:into: variant ---
    // Same shape as kCountedLoopInjectInto but iterates a virtual
    // Interval directly — start..stop as SmI integers, no basicAt
    // helper call per element.  Used when the lifter saw `to:`
    // followed by a recognized inject:into: pattern.
    //
    // operands: start, stop, init  -> Oop  (final acc)
    // literal:  block-IR slot
    kCountedLoopIntervalInjectInto,

    // --- B2 splice: (start to: stop) do: variant ---
    // Counted loop iterating SmI integers start..stop with the block
    // body inlined.  No accumulator (do: discards its block result).
    // Result on stack is a placeholder for the Interval (rarely
    // observed; if it is, deopts on use).
    //
    // operands: start, stop          -> Oop  (placeholder for the Interval)
    // literal:  block-IR slot
    kCountedLoopIntervalDo,

    // --- B2 splice: Array do: closure accumulator variant ---
    // Recognized at bytecode level (no IR sub-lift): block has the
    // exact 5-instruction shape `pushTempAtInVec(slot,1); pushTemp 0;
    // <arith>; storeTempAtInVec(slot,1); blockReturn`, with numCopied=1.
    // The outer pushed the TempVector at outer-temp T just before
    // PushFullBlock; the splice captures T as a separate operand.
    //
    // Mid-loop deopt safety: accReg is held in a register only; vec[slot]
    // stays untouched until SUCCESSFUL loop exit, so deopt rolls back to
    // PushFullBlock with vec[slot] still holding the original "s".
    //
    // operands: rcv (the Array), vecRef (the TempVector Oop)  -> Oop (rcv)
    // literal:  bits 0-15  = vec slot index
    //           bits 16-23 = arith op (0=add, 1=sub, 2=mul)
    //           bits 24+  = (reserved)
    kCountedLoopArrayDoAccum,

    // --- B2 splice: (start to: stop) do: closure accumulator variant ---
    // Like kCountedLoopArrayDoAccum but the receiver is a SmI Interval
    // (start, stop) instead of an Array.  Iterates i = start..stop and
    // accumulates into vec[slot] using the recognized arith op.
    //
    // Deopt safety: accReg held in a register; vec[slot] only stored on
    // successful exit.
    //
    // operands: start, stop, vecRef  -> Oop (placeholder for the Interval)
    // literal:  same packed layout as kCountedLoopArrayDoAccum
    //           bits 0-15  = vec slot index
    //           bits 16-23 = arith op (0=add, 1=sub, 2=mul)
    kCountedLoopIntervalDoAccum,

    // --- B2 splice: arr collect: [:e | expr] ---
    // Recognized at bytecode level: Send2 #collect: where receiver is
    // an Array (runtime tag-checked) and the block is splice-simple
    // (LoadTemp 0 + arith ops + LoadConst + BlockReturn).
    //
    // Lowering allocates a fresh Array of the receiver's size, iterates
    // i = 1..size loading arr[i], applies the inlined block body
    // producing the result element, and stores into result[i].
    // Returns the new Array.
    //
    // Mid-loop deopt: rebuilds [arr, FullBlock] and resumes at the
    // Send2 #collect: bytecode.  The interpreter re-runs collect: from
    // scratch (allocates a fresh result, ignoring the partial work).
    //
    // operands: rcv (the Array)  -> Oop (the new Array)
    // literal:  block-IR slot (low 32 bits)
    kCountedLoopArrayCollect,

    // Pharo-inlined `n timesRepeat: [outer := outer arith const]`
    // (and `(start to: stop) do:` with literal bounds).  Pharo's
    // bytecode compiler inlines these as counted whileTrue: loops with
    // no Send to #timesRepeat:/#do:, so the splice family above can't
    // intercept them.  This op replaces the entire inlined-loop
    // bytecode range when the body is the canonical 4-byte sequence
    // `pushTemp X, pushOne (or 0), arithBase Y, popTemp X`.  Math
    // simplifies to `outer = outer (+/-) (limit - countInit + 1) * const`.
    //
    // operands: (none — all metadata in literal)
    // literal:
    //   bits  0-7   = accumTempIdx (the body's accum temp)
    //   bits  8-11  = arithCode (0=Add, 1=Sub)
    //   bits 12-19  = constValue (signed 8-bit; from PushZero/PushOne)
    //   bits 20-27  = limit_litIdx (literal index of LIMIT in method)
    //   bits 28-35  = countTempIdx (the loop counter temp)
    //   bits 36-43  = countInitValue (signed 8-bit, typically 1)
    //   bits 44-63  = bcOffset of pre-loop start (deopt resume)
    //
    // Type::kOop placeholder result; the lifter consumes the entire
    // pattern bytecode-wise (advancing ip past the post-loop pop), so
    // no simulator stack effect.  See SistaBuilder pre-pass and the
    // SistaLowering case for exact bytecode layout.
    kCountedLoopWhileTrueAccum,

    // Pharo-inlined `n timesRepeat: [body]` where body contains K
    // side-effect-discarded sends BEFORE the canonical 4-byte
    // counter-arith — e.g., runInstVar's `[obj size. obj yourself]`:
    //
    //   pushTemp obj; sendSel0; pop;
    //   ...
    //   pushTemp obj; sendSelK-1; pop;
    //   pushTemp counter; pushOne; ArithBase Y; popTemp counter
    //
    // Each leading send must be IC-monomorphic with a class that the
    // selector inlines safely (Object>>yourself = no-op, multi-slot
    // getter shape, etc.).  Lowering emits a real iteration loop —
    // the math shortcut isn't applicable when the body has sends.
    //
    // operands: 1 (the receiver obj loaded once before loop)
    // literal: same low-44 bits as kCountedLoopWhileTrueAccum +
    //   bits 60-63 = body kind:
    //     1 = pushTemp+yourself+pop pair (any class, no guard)
    //     (more kinds added as recognizers extend)
    kCountedLoopBodyExec,

    // --- Phi ---
    // SSA merge at a block with multiple predecessors.  Operand[i]
    // is the incoming value from Block::predecessors[i] (same order).
    // Emitted at block entry (before any other op) for every stack
    // slot live at entry.
    kPhi,                 // operands: inc_0, inc_1, ...                   -> any

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
    // Simulated-stack contents at the block's terminator, in bottom-up
    // order.  Populated by the lifter so phi wiring can read them.
    // Both successors of a conditional branch see the same outgoing
    // stack (the cond is already popped), so one vector suffices.
    std::vector<uint32_t> outgoingStack;
};

// ===== Method (IR-level) =====
//
// One compiled method's IR.  Constructed by SistaBuilder, consumed by
// SistaLowering (and optionally transformed by SistaInliner / peephole
// passes in between).

// ===== InlineHint =====
//
// Phase 4 Step 1: profile-guided inlining.  T1's inline cache (IC)
// records (receiver class, target method) per send site after warmup.
// Sista queries this at compile time to identify monomorphic sites
// that are candidates for inlining.  Each hint maps a source
// bytecode offset to the IC's observed class + method.
//
// Today (this commit): hints are plumbed but not USED — Builder
// just records how many sends would be candidates.  Step 2 lifts
// the callee, Step 3 splices it, Step 4 adds heuristics.
struct InlineHint {
    uint16_t bcOffset;       // Source bytecode position of the send
    uint64_t classOop;       // Observed receiver class (raw bits)
    uint64_t targetMethod;   // Resolved CompiledMethod oop (raw bits)
};

// ===== Framepoint =====
//
// PHASE 3 deopt infrastructure (started 2026-04-24).  At every
// potential deopt site (currently every kSendUnspeculated; later
// kGuardClass, kPrimTagCheckInt, etc.), the builder snapshots the
// interpreter-visible state needed to reconstruct a bail.
//
// For monomorphic inlining (Phase 4), the deopt landing pad of an
// inlined callee will reuse the framepoint of the OUTER send to
// recover caller-frame state, then re-execute the unspeculated send
// to re-do the call in the interpreter.
//
// Today (Step 1): emitted alongside every kSendUnspeculated; not yet
// consumed.  Existing ExitSend bail mechanism still drives recovery.
struct Framepoint {
    uint32_t valueId;                    // The kSendUnspeculated value
                                          // this framepoint describes
    uint16_t bcOffset;                   // Bytecode index to resume at
    std::vector<uint32_t> stackValueIds; // IR value IDs whose contents
                                          // form the simulated stack at
                                          // this point, bottom→top
    // Future: temp value IDs, IR-value → physical-slot map (post
    // register allocation).
};

struct Method {
    Oop compiledMethodOop = Oop::nil(); // Original CompiledMethod
    std::vector<Value> values;           // All values, indexed by id
    std::vector<Block> blocks;           // All blocks, indexed by id
    std::vector<Oop>   literals;         // Cached literal table (GC-tracked)
    std::vector<Framepoint> framepoints; // Phase 3 deopt info; one per
                                          // potential deopt site (today
                                          // = every kSendUnspeculated).
    // B2 splice: inlined block bodies referenced by kCountedLoopDo
    // (and any future inline-block ops).  Each entry is a fully
    // sub-lifted Method holding the block's IR.  The lowering walks
    // this IR when emitting the spliced loop body.
    //
    // unique_ptr because Method holds vectors of Method (recursive),
    // which would either grow unbounded or copy expensively.
    std::vector<std::unique_ptr<Method>> inlinedBlocks;
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
