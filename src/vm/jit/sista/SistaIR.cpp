/*
 * SistaIR.cpp - Minimal implementations for the Sista IR.
 *
 * Builder helpers and a textual pretty-printer.  No code generation
 * here — that belongs in SistaLowering.cpp once it exists.
 */
#include "SistaIR.hpp"

#include <cstdio>
#include <sstream>

namespace pharo {
namespace sista {

// ===== Op metadata =====

namespace OpInfo {

bool isTerminator(Op op) {
    switch (op) {
    case Op::kBranch:
    case Op::kBranchIfTrue:
    case Op::kBranchIfFalse:
    case Op::kReturn:
    case Op::kNonLocalReturn:
        return true;
    default:
        return false;
    }
}

bool producesOop(Op op) {
    switch (op) {
    case Op::kConstantOop:
    case Op::kLoadReceiver:
    case Op::kLoadTrueOop:
    case Op::kLoadFalseOop:
    case Op::kLoadArg:
    case Op::kLoadTemp:
    case Op::kLoadInstVar:
    case Op::kLoadLiteral:
    case Op::kPrimAddInt:
    case Op::kPrimSubInt:
    case Op::kPrimMulInt:
    case Op::kPrimLtInt:
    case Op::kPrimLeInt:
    case Op::kPrimGtInt:
    case Op::kPrimEqInt:
    case Op::kSendUnspeculated:
    case Op::kGuardClass:
    case Op::kInlineSend:
    case Op::kBlockCreate:
    case Op::kBlockValue:
    case Op::kPhi:
        return true;
    default:
        return false;
    }
}

bool mayCallBack(Op op) {
    switch (op) {
    case Op::kSendUnspeculated:
    case Op::kGuardClass:       // miss deopts
    case Op::kInlineSend:       // nested send
    case Op::kBlockValue:
    case Op::kPrimTagCheckInt:  // miss deopts
        return true;
    default:
        return false;
    }
}

bool readsStack(Op op) {
    // Currently the IR doesn't model stack reads explicitly — loads
    // go via kLoadTemp / kLoadReceiver / kLoadArg.  Placeholder for
    // when we add explicit stack-slot accesses (Phase 3 deopt support).
    (void)op;
    return false;
}

bool writesStack(Op op) {
    (void)op;
    return false;
}

const char* name(Op op) {
    switch (op) {
    case Op::kConstantOop:      return "const_oop";
    case Op::kConstantInt:      return "const_int";
    case Op::kLoadReceiver:     return "load_recv";
    case Op::kLoadTrueOop:      return "load_true";
    case Op::kLoadFalseOop:     return "load_false";
    case Op::kLoadArg:          return "load_arg";
    case Op::kLoadTemp:         return "load_temp";
    case Op::kStoreTemp:        return "store_temp";
    case Op::kLoadInstVar:      return "load_ivar";
    case Op::kStoreInstVar:     return "store_ivar";
    case Op::kLoadLiteral:      return "load_lit";
    case Op::kBranch:           return "branch";
    case Op::kBranchIfTrue:     return "branch_if_true";
    case Op::kBranchIfFalse:    return "branch_if_false";
    case Op::kReturn:           return "return";
    case Op::kNonLocalReturn:   return "nlr";
    case Op::kSendUnspeculated: return "send";
    case Op::kGuardClass:       return "guard_class";
    case Op::kInlineSend:       return "inline_send";
    case Op::kPrimAddInt:       return "prim_add_int";
    case Op::kPrimSubInt:       return "prim_sub_int";
    case Op::kPrimMulInt:       return "prim_mul_int";
    case Op::kPrimLtInt:        return "prim_lt_int";
    case Op::kPrimLeInt:        return "prim_le_int";
    case Op::kPrimGtInt:        return "prim_gt_int";
    case Op::kPrimEqInt:        return "prim_eq_int";
    case Op::kPrimTagCheckInt:  return "tag_check_int";
    case Op::kBlockCreate:      return "block_create";
    case Op::kBlockValue:       return "block_value";
    case Op::kPhi:              return "phi";
    case Op::kFrameState:       return "frame_state";
    }
    return "???";
}

}  // namespace OpInfo

static const char* typeName(Type t) {
    switch (t) {
    case Type::kVoid:        return "void";
    case Type::kOop:         return "Oop";
    case Type::kOopSmallInt: return "OopSmallInt";
    case Type::kOopChar:     return "OopChar";
    case Type::kOopBool:     return "OopBool";
    case Type::kInt:         return "Int";
    case Type::kBool:        return "Bool";
    case Type::kFrameState:  return "FrameState";
    }
    return "???";
}

// ===== Method helpers =====

uint32_t Method::newBlock(int32_t sourceBc) {
    Block b;
    b.id = static_cast<uint32_t>(blocks.size());
    b.sourceBytecodeOffset = sourceBc;
    blocks.push_back(std::move(b));
    return blocks.back().id;
}

uint32_t Method::newValue(uint32_t blockId, Op op, Type type,
                           std::vector<uint32_t> operands, uint64_t literal) {
    Value v;
    v.id       = static_cast<uint32_t>(values.size());
    v.op       = op;
    v.type     = type;
    v.block    = static_cast<uint16_t>(blockId);
    v.operands = std::move(operands);
    v.literal  = literal;
    values.push_back(std::move(v));
    blocks[blockId].values.push_back(values.back().id);
    return values.back().id;
}

void Method::addEdge(uint32_t from, uint32_t to) {
    blocks[from].successors.push_back(to);
    blocks[to].predecessors.push_back(from);
}

// ===== Pretty printing =====

std::string Method::toString() const {
    std::ostringstream os;
    os << "SistaMethod(compiledMethodOop=0x" << std::hex
       << compiledMethodOop.rawBits() << std::dec
       << ", args=" << numArgs << ", temps=" << numTemps
       << ", blocks=" << blocks.size()
       << ", values=" << values.size() << ")\n";
    for (const Block& b : blocks) {
        os << "  block " << b.id;
        if (b.sourceBytecodeOffset >= 0)
            os << " [bc=" << b.sourceBytecodeOffset << "]";
        if (!b.predecessors.empty()) {
            os << "  preds={";
            for (size_t i = 0; i < b.predecessors.size(); i++) {
                if (i) os << ",";
                os << b.predecessors[i];
            }
            os << "}";
        }
        os << "\n";
        for (uint32_t vid : b.values) {
            const Value& v = values[vid];
            os << "    v" << v.id << " : " << typeName(v.type)
               << " = " << OpInfo::name(v.op);
            for (uint32_t o : v.operands)
                os << " v" << o;
            if (v.literal != 0)
                os << "  lit=0x" << std::hex << v.literal << std::dec;
            os << "\n";
        }
        if (!b.successors.empty()) {
            os << "    -> ";
            for (size_t i = 0; i < b.successors.size(); i++) {
                if (i) os << ", ";
                os << b.successors[i];
            }
            os << "\n";
        }
    }
    return os.str();
}

}  // namespace sista
}  // namespace pharo
