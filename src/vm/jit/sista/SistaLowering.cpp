/*
 * SistaLowering.cpp - Sista IR -> native code
 *
 * Stub for Phase 2 completion.  See SistaLowering.hpp for the interface.
 *
 * Deliberately empty until the builder covers enough bytecodes to
 * make lowering worth wiring.  Compiles so the header stays linked
 * and imports resolve; returns false so any caller knows the
 * feature isn't available yet.
 */
#include "SistaLowering.hpp"

namespace pharo {
namespace sista {

bool lower(const Method& method, CodeSink& sink) {
    (void)method;
    sink.written = 0;
    return false;  // Unimplemented — caller should fall back to Tier 1.
}

}  // namespace sista
}  // namespace pharo
