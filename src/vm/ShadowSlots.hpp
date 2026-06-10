// ShadowSlots.hpp — visibility-independent corruption detector
// (PHARO_SHADOW_SLOTS).
//
// Mirrors every TRACKED pointer-slot write into a shadow table keyed by
// (object, slot); receiver-ivar READS verify the live slot against the
// shadow.  A mismatch means the slot changed through an untracked path:
// either a writer we haven't instrumented yet (extend the tracking) or
// — the central suspect of the 2026-06 J2J ivar-store corruption hunt —
// a GC-mover bug that rewrites slots inconsistently.  Unlike the DNU
// symptom, this fires on ANY layout (the corruption's visibility is a
// layout knife-edge; see WIP.md "EPISTEMOLOGY").
//
// Tracked writers: the three JIT receiver-store emits (via
// jit_rt_store_ring), Interpreter::setReceiverInstVar, and
// ObjectMemory::storePointer.  Untracked-by-design: raw memcpy object
// copies (fresh identities — no stale entries), basicNew nil-fills
// (fresh), image load.  become:/exchange flows clear the table.
//
// GC: forEachRoot visits entry obj+value oops (like countMap_) and
// afterGC rehashes (positions go stale when keys move).
//
// Entries are evicted on probe-window overflow — a lookup miss simply
// skips verification (coverage, not correctness).
#ifndef PHARO_SHADOW_SLOTS_HPP
#define PHARO_SHADOW_SLOTS_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pharo {

struct ShadowEntry {
    uint64_t obj;     // object oop bits (0 = empty slot)
    uint64_t slot;    // slot index
    uint64_t value;   // last tracked value written
    uint64_t writer;  // provenance: 1=interp setReceiverInstVar,
                      // 2=storePointer, else JITMethod* of the emit site
};

constexpr size_t kShadowSize = 1 << 20;   // 1M entries, 32 MB
constexpr size_t kShadowProbes = 4;

extern ShadowEntry g_shadowSlots[kShadowSize];
extern uint64_t g_shadowStores;
extern uint64_t g_shadowChecks;
extern uint64_t g_shadowMismatches;

inline size_t shadowIdx(uint64_t obj, uint64_t slot) {
    return (((obj >> 3) * 0x9E3779B97F4A7C15ULL) ^ slot) & (kShadowSize - 1);
}

inline void shadowStore(uint64_t obj, uint64_t slot, uint64_t value,
                        uint64_t writer) {
    size_t idx = shadowIdx(obj, slot);
    for (size_t p = 0; p < kShadowProbes; p++) {
        ShadowEntry& e = g_shadowSlots[(idx + p) & (kShadowSize - 1)];
        if (e.obj == obj && e.slot == slot) {
            e.value = value;
            e.writer = writer;
            g_shadowStores++;
            return;
        }
        if (e.obj == 0) {
            e.obj = obj; e.slot = slot; e.value = value; e.writer = writer;
            g_shadowStores++;
            return;
        }
    }
    // Window full — evict the first probe slot.
    ShadowEntry& e = g_shadowSlots[idx];
    e.obj = obj; e.slot = slot; e.value = value; e.writer = writer;
    g_shadowStores++;
}

inline ShadowEntry* shadowLookup(uint64_t obj, uint64_t slot) {
    size_t idx = shadowIdx(obj, slot);
    for (size_t p = 0; p < kShadowProbes; p++) {
        ShadowEntry& e = g_shadowSlots[(idx + p) & (kShadowSize - 1)];
        if (e.obj == obj && e.slot == slot) return &e;
        if (e.obj == 0) return nullptr;
    }
    return nullptr;
}

inline void shadowClear() {
    std::memset(g_shadowSlots, 0, sizeof(ShadowEntry) * kShadowSize);
}

// Verify a read against the shadow.  Returns true on mismatch (already
// reported).  `where` names the read site for the report.
bool shadowVerify(uint64_t obj, uint64_t slot, uint64_t actual,
                  const char* where);

// Rehash after GC (keys updated in place by forEachRoot).
void shadowRehashAfterGC();

}  // namespace pharo

#endif  // PHARO_SHADOW_SLOTS_HPP
