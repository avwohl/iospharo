// ShadowSlots.cpp — storage + mismatch reporting for the shadow-slot
// verify-on-read corruption detector.  See ShadowSlots.hpp.
#include "ShadowSlots.hpp"
#include "DebugVars.hpp"

namespace pharo {

ShadowEntry g_shadowSlots[kShadowSize] = {};
uint64_t g_shadowStores = 0;
uint64_t g_shadowChecks = 0;
uint64_t g_shadowMismatches = 0;

bool shadowVerify(uint64_t obj, uint64_t slot, uint64_t actual,
                  const char* where) {
    g_shadowChecks++;
    ShadowEntry* e = shadowLookup(obj, slot);
    if (!e) return false;            // untracked slot — no claim
    if (e->value == actual) return false;
    g_shadowMismatches++;
    static int reported = 0;
    if (++reported <= 40) {
        fprintf(stderr,
            "[SHADOW-MISMATCH #%d] %s obj=0x%llx slot=%llu "
            "shadow=0x%llx actual=0x%llx writer=0x%llx "
            "(checks=%llu stores=%llu)\n",
            reported, where,
            (unsigned long long)obj, (unsigned long long)slot,
            (unsigned long long)e->value, (unsigned long long)actual,
            (unsigned long long)e->writer,
            (unsigned long long)g_shadowChecks,
            (unsigned long long)g_shadowStores);
    }
    // Re-sync so one corruption doesn't spam every subsequent read.
    e->value = actual;
    return true;
}

// Rehash after GC: keys were updated in place by forEachRoot, so hash
// positions are stale.  Heap-allocates the 32MB temp (diagnostic-only
// path; mirrors recoverAfterGC's countMap_ pattern).
void shadowRehashAfterGC() {
    auto* temp = new ShadowEntry[kShadowSize];
    std::memcpy(temp, g_shadowSlots, sizeof(ShadowEntry) * kShadowSize);
    std::memset(g_shadowSlots, 0, sizeof(ShadowEntry) * kShadowSize);
    for (size_t i = 0; i < kShadowSize; i++) {
        if (temp[i].obj != 0) {
            shadowStore(temp[i].obj, temp[i].slot, temp[i].value,
                        temp[i].writer);
        }
    }
    delete[] temp;
}

}  // namespace pharo
