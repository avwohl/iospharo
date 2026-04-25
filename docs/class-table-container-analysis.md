# Class table container — analysis & recommendation (2026-04-25)

## Question

Should we replace `std::vector<Oop> classTable_` (4M slots = 32 MB)
with a `std::unordered_map<uint32_t, Oop>` or other container?

## Architecture today

The class table maps `classIndex (22-bit, == identity hash in Spur)
→ classOop`.  Two parallel storages:

  1. **C++ `classTable_`**: 4M-slot `std::vector<Oop>` —
     runtime cache, hot read path.
  2. **In-heap pages** (`classTablePages_`): Pharo image's own
     representation, 1024 entries per page, ~5-10 pages active.
     Used for image save (`syncClassTableToHeap()`) and GC.

The two stay in sync: `registerClass()` updates only the C++
vector; `syncClassTableToHeap()` flushes back to heap pages
before image save.

## Usage profile

**Hot reads** (every send dispatch + every type check):
  - `classAtIndex(idx)` → 1 bounds check + 1 vector load
  - Called by `classOf()` from interpreter dispatch loop
  - Estimated 10-100M calls/sec on active workloads

**Cold writes** (rare):
  - `registerClass()` — new anonymous class created
  - Image load — bulk populate
  - `setClassAtIndex()` — explicit (e.g., context-class repair)

**Iteration** (per-GC):
  - `forEachMemoryRoot` — visit all non-nil entries
  - nil-propagation after GC — scan all entries
  - `indexOfClass` — linear scan (rare)
  - `generateHash()` collision check — single index lookup

**Density**: ~5K real classes / 4M slots = 0.12 % occupied →
~32 MB resident, ~99.88 % nil.

## Container comparison

    Container                        Read       Write      Memory      Iteration       Comment
    ---------------------------     ---------  ---------  ----------  ---------------  ------------------
    std::vector<Oop> 4M (current)    1 load     1 store    32 MB       full scan        wasteful but FAST
    std::unordered_map<u32, Oop>     hash+walk  hash+walk  ~120 KB     occupied only    ~5-10× slower read
    std::vector<Oop> nextIdx + grow  1 load     1 store    ~40 KB      occupied only    breaks Spur indices
    Sparse paged vector (custom)     2 loads    2 stores   ~80 KB      pages × slots    +1 indirection
    Read-from-in-heap directly       2-3 loads  via heap   0 (dup)     existing pages   needs GC discipline

## Recommendation: KEEP `std::vector<Oop>` at 4M slots

### Why

1. **Perf trumps memory at this size.**  `classAtIndex` is on
   the *hottest* read path in the VM.  A `std::unordered_map`
   lookup costs 5-10× more (hash + bucket-walk + cmp vs single
   indexed load), and this multiplier hits 10M+ times per
   second on real workloads.  32 MB is noise on a modern Mac
   with 8-32 GB; the perf hit isn't.

2. **The collision-avoidance dance is cheap.**  Today's
   `generateHash()` check (`classTable_[hash].isObject()` —
   commit `0ca2b58`) runs only at identity-hash assignment
   time, not on every `classOf()` call.  Skip rate is ~0.12 %
   so the LCG re-roll is rare.  No real cost.

3. **Spur compat constrains alternatives.**  In Spur, a class's
   identity hash IS its class table index.  Anything that
   renumbers classes on the fly breaks image-side code that
   stored references via identity hash (IdentitySets, etc.).
   So we're stuck with 22-bit hash space → class table needs
   22-bit-keyed lookup.  That points back at either a 4M-slot
   vector or a hash map.

### When to reconsider

Switch only if one of these becomes true:

  - **Embedded target with tight RAM budget**: 32 MB might
    matter on iOS / iPad-class hardware, but per the
    `feedback_target_is_macos.md` memory entry, this VM is
    Mac-only.
  - **Profiler shows classAtIndex measurable**: today's
    profile (`docs/perf-2026-04-24-yg-default/profile-active-yg-default.txt`)
    shows `classOf` at 904 samples (1.9 % of CPU).
    `classAtIndex` is inside that.  Cutting it 50 % via a
    smaller hot footprint isn't worth a 5× slowdown of
    every call.
  - **The sparse-paged vector idea matures**: if we want
    both small footprint AND fast access, a custom 1024-slot
    paged vector can give it.  Pages allocated on demand,
    1 extra indirection.  ~80 KB total.  Worth a half-day of
    work IF memory becomes a constraint.

### What to do today

Nothing.  The current vector is correct, fast, and sufficient.
Update `src/vm/ObjectMemory.hpp` to remove the "TODO
replace with std::map" hint added in commit `045bccb` —
that suggestion would be a regression.

## Alternative: read-from-in-heap

This is intriguing but has invariant complications:

- `classTablePages_` already holds the pages
- Each page is a Pharo Array object, GC-managed
- `classAtIndex(idx)` would compute `pageNum = idx / 1024`,
  then load page Oop from `classTablePages_[pageNum]`,
  then `slotNum = idx % 1024`, then load slot from page
- 3 dependent loads vs 1
- Avoids duplicate storage (32 MB vector + heap pages)
- Eliminates `syncClassTableToHeap()` entirely

Caveat: every GC compaction moves page objects.  We already
track these via `classTablePages_` for that reason.  The
in-heap-direct approach would be FRAGILE — any code path
that writes a class entry must remember to update via
`page->slotAtPut(...)` not `classTable_[idx] = ...`.

This is the right thing IF we want minimum memory + simplicity.
But the 3-load-deep dependency chain on the hot path is
likely worse than the current 1-load.  Need to measure.

## Decision

Keep current.  Update the TODO note to reflect this analysis.
