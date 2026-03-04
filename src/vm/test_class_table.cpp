/*
 * test_class_table.cpp - Class table integrity tests
 *
 * Tests that class table pages survive GC compaction and image save/reload.
 * These tests would have caught the bugs fixed in Build 70:
 *   1. Class table pages not marked during GC
 *   2. Page pointers not updated after compaction
 *   3. New classes not synced to in-heap pages
 *
 * Usage: ./test_class_table <path-to-Pharo.image>
 */

#include "ObjectMemory.hpp"
#include "ObjectHeader.hpp"
#include "ImageLoader.hpp"
#include "ImageWriter.hpp"
#include "Oop.hpp"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>   // access()

using namespace pharo;

static int failures = 0;
static int passes = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << msg << std::endl; \
        failures++; \
    } else { \
        std::cout << "  PASS: " << msg << std::endl; \
        passes++; \
    } \
} while(0)

// ===== HELPERS =====

/// Walk hiddenRootsObj and return the class table page objects.
/// Returns the number of non-nil pages found.
struct ClassTablePageInfo {
    ObjectHeader* page;
    size_t pageNum;
    size_t slotCount;
};

/// Returns only real class table pages (1024-slot pointer Arrays).
/// hiddenRoots also has extra root objects (remembered sets, etc.) that
/// have different formats/sizes — those are excluded.
static std::vector<ClassTablePageInfo> getClassTablePages(ObjectMemory& memory) {
    std::vector<ClassTablePageInfo> pages;
    Oop hrOop = memory.hiddenRootsObj();
    if (!hrOop.isObject()) return pages;

    ObjectHeader* hr = hrOop.asObjectPtr();
    size_t hrSlots = hr->slotCount();
    constexpr size_t MaxPages = 4096;

    for (size_t i = 0; i < hrSlots && i < MaxPages; ++i) {
        Oop pageOop = hr->slotAt(i);
        if (!pageOop.isObject()) continue;
        if (pageOop == memory.nil()) continue;
        if (pageOop.rawBits() == 0) continue;

        ObjectHeader* page = pageOop.asObjectPtr();
        // Only count real class table pages (1024-slot pointer objects).
        // Higher-numbered slots may be extra roots (remembered sets, etc.)
        // with different formats.
        if (page->slotCount() == 1024) {
            pages.push_back({page, i, page->slotCount()});
        }
    }
    return pages;
}

/// Read a class entry from the in-heap page at (pageNum, slotNum).
static Oop readHeapClassEntry(ObjectMemory& memory, size_t classIndex) {
    constexpr size_t PageSize = 1024;
    size_t pageNum = classIndex / PageSize;
    size_t slotNum = classIndex % PageSize;

    Oop hrOop = memory.hiddenRootsObj();
    if (!hrOop.isObject()) return Oop::nil();

    ObjectHeader* hr = hrOop.asObjectPtr();
    if (pageNum >= hr->slotCount()) return Oop::nil();

    Oop pageOop = hr->slotAt(pageNum);
    if (!pageOop.isObject() || pageOop == memory.nil()) return Oop::nil();

    ObjectHeader* page = pageOop.asObjectPtr();
    if (slotNum >= page->slotCount()) return Oop::nil();

    return page->slotAt(slotNum);
}

// ===== TEST 1: Class table pages exist after image load =====

static void testClassTablePagesExist(ObjectMemory& memory) {
    std::cout << "\n=== Test: Class table pages exist after load ===" << std::endl;

    Oop hrOop = memory.hiddenRootsObj();
    CHECK(hrOop.isObject(), "hiddenRootsObj is a valid object");

    ObjectHeader* hr = hrOop.asObjectPtr();
    CHECK(hr->slotCount() >= 4096, "hiddenRootsObj has >= 4096 slots");

    auto pages = getClassTablePages(memory);
    CHECK(pages.size() > 0, "At least one class table page exists");
    CHECK(pages.size() >= 4, "At least 4 pages (Pharo 13 uses ~6)");

    // Page 0 should contain immediate type classes
    if (!pages.empty() && pages[0].pageNum == 0) {
        CHECK(pages[0].slotCount == 1024, "Page 0 has 1024 slots");
    }

    std::cout << "  Found " << pages.size() << " class table pages" << std::endl;
}

// ===== TEST 2: C++ classTable_ matches in-heap pages =====

static void testClassTableConsistency(ObjectMemory& memory) {
    std::cout << "\n=== Test: C++ classTable matches in-heap pages ===" << std::endl;

    int mismatches = 0;
    int checked = 0;
    int nilInHeap = 0;

    // Check first 8192 entries (covers pages 0-7)
    for (size_t i = 1; i < 8192; ++i) {
        Oop cppEntry = memory.classAtIndex(static_cast<uint32_t>(i));
        Oop heapEntry = readHeapClassEntry(memory, i);

        if (!cppEntry.isObject() && !heapEntry.isObject()) continue;
        if (cppEntry == memory.nil() && heapEntry == memory.nil()) continue;
        if (cppEntry == memory.nil() && !heapEntry.isObject()) continue;
        if (!cppEntry.isObject() && heapEntry == memory.nil()) continue;

        checked++;

        if (cppEntry.rawBits() != heapEntry.rawBits()) {
            if (heapEntry == memory.nil() || !heapEntry.isObject()) {
                nilInHeap++;
            }
            mismatches++;
            if (mismatches <= 5) {
                std::cerr << "    Mismatch at index " << i
                          << ": C++=" << std::hex << cppEntry.rawBits()
                          << " heap=" << heapEntry.rawBits() << std::dec << std::endl;
            }
        }
    }

    // After a fresh load (no mutations), they should match exactly
    CHECK(mismatches == 0, "C++ classTable matches in-heap pages (" +
          std::to_string(checked) + " checked, " +
          std::to_string(mismatches) + " mismatches)");
}

// ===== TEST 3: Class table pages survive fullGC =====

static void testClassTableSurvivesGC(ObjectMemory& memory) {
    std::cout << "\n=== Test: Class table pages survive fullGC ===" << std::endl;

    // Count classes and sample entries before GC
    int classesBefore = 0;
    std::vector<std::pair<uint32_t, Oop>> samplesBefore;
    for (uint32_t i = 1; i < 8192; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.rawBits() != 0 && cls != memory.nil() && cls.isObject()) {
            classesBefore++;
            if (samplesBefore.size() < 200) {
                samplesBefore.push_back({i, cls});
            }
        }
    }
    CHECK(classesBefore > 0, std::to_string(classesBefore) + " classes before GC");

    auto pagesBefore = getClassTablePages(memory);

    // Run full GC (includes compaction)
    GCResult gcResult = memory.fullGC();
    std::cout << "  GC: reclaimed " << gcResult.bytesReclaimed
              << " bytes, moved " << gcResult.objectsMoved << " objects" << std::endl;

    // Verify class count unchanged
    int classesAfter = 0;
    for (uint32_t i = 1; i < 8192; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.rawBits() != 0 && cls != memory.nil() && cls.isObject()) {
            classesAfter++;
        }
    }
    CHECK(classesAfter == classesBefore,
          "Same class count after GC (" +
          std::to_string(classesAfter) + " vs " +
          std::to_string(classesBefore) + ")");

    // Verify pages still exist (at least as many real pages)
    auto pagesAfter = getClassTablePages(memory);
    CHECK(pagesAfter.size() >= pagesBefore.size() - 2,
          "Class table pages survived GC (" +
          std::to_string(pagesAfter.size()) + " after, " +
          std::to_string(pagesBefore.size()) + " before)");

    // Verify sampled class entries still resolve correctly
    int classLost = 0;
    for (auto& [idx, oldOop] : samplesBefore) {
        Oop newOop = memory.classAtIndex(idx);
        if (newOop.rawBits() == 0 || newOop == memory.nil() || !newOop.isObject()) {
            classLost++;
            if (classLost <= 3) {
                std::cerr << "    Class at index " << idx << " lost after GC"
                          << " (was 0x" << std::hex << oldOop.rawBits()
                          << ", now 0x" << newOop.rawBits() << std::dec << ")" << std::endl;
            }
        }
    }
    CHECK(classLost == 0, "No classes lost after GC (" +
          std::to_string(samplesBefore.size()) + " sampled)");
}

// ===== TEST 4: syncClassTableToHeap updates in-heap pages =====

static void testSyncClassTableToHeap(ObjectMemory& memory) {
    std::cout << "\n=== Test: syncClassTableToHeap writes new entries ===" << std::endl;

    // Find a class that exists in C++ table (use low indices where classes exist)
    Oop testClass;
    uint32_t testIndex = 0;
    for (uint32_t i = 10; i < 200; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.rawBits() != 0 && cls != memory.nil() && cls.isObject()) {
            testClass = cls;
            testIndex = i;
            break;
        }
    }
    CHECK(testIndex > 0, "Found a test class at index " + std::to_string(testIndex));
    if (testIndex == 0) return;

    // Find an unused slot to test registration.
    // Search backwards from testIndex's page to find an empty slot within
    // a page that exists (so syncClassTableToHeap can write to it).
    // Class indices within a page's range but not used will have Oop(0) or nil.
    uint32_t unusedIndex = 0;
    constexpr size_t PageSize = 1024;
    size_t testPage = testIndex / PageSize;
    uint32_t pageStart = static_cast<uint32_t>(testPage * PageSize);
    for (uint32_t i = pageStart + 900; i > pageStart + 500; --i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.rawBits() == 0 || cls == memory.nil()) {
            unusedIndex = i;
            break;
        }
    }
    CHECK(unusedIndex > 0, "Found unused class index " + std::to_string(unusedIndex));
    if (unusedIndex == 0) return;

    // Manually set a class in the C++ table (simulating registerClass)
    memory.setClassAtIndex(unusedIndex, testClass);

    // Sync
    memory.syncClassTableToHeap();

    // After sync: heap SHOULD have this entry
    Oop heapAfter = readHeapClassEntry(memory, unusedIndex);
    CHECK(heapAfter.rawBits() == testClass.rawBits(),
          "Heap entry matches after syncClassTableToHeap");

    // Clean up: remove the fake entry
    memory.setClassAtIndex(unusedIndex, memory.nil());
    memory.syncClassTableToHeap();
}

// ===== TEST 5: Class table survives fullGC + sync roundtrip =====

static void testClassTableSurvivesGCWithSync(ObjectMemory& memory) {
    std::cout << "\n=== Test: Class table survives GC + sync roundtrip ===" << std::endl;

    // Sync first (ensure heap matches C++)
    memory.syncClassTableToHeap();

    // Run GC (compaction may move pages)
    GCResult gcResult = memory.fullGC();
    std::cout << "  GC: reclaimed " << gcResult.bytesReclaimed
              << " bytes, moved " << gcResult.objectsMoved << " objects" << std::endl;

    // Verify consistency after GC
    int mismatches = 0;
    int checked = 0;
    for (size_t i = 1; i < 8192; ++i) {
        Oop cppEntry = memory.classAtIndex(static_cast<uint32_t>(i));
        Oop heapEntry = readHeapClassEntry(memory, i);

        if (!cppEntry.isObject() && !heapEntry.isObject()) continue;
        if (cppEntry == memory.nil() && (heapEntry == memory.nil() || !heapEntry.isObject())) continue;
        if (!cppEntry.isObject() && heapEntry == memory.nil()) continue;

        checked++;
        if (cppEntry.rawBits() != heapEntry.rawBits()) {
            mismatches++;
            if (mismatches <= 5) {
                std::cerr << "    Mismatch at index " << i
                          << ": C++=" << std::hex << cppEntry.rawBits()
                          << " heap=" << heapEntry.rawBits() << std::dec << std::endl;
            }
        }
    }

    CHECK(mismatches == 0, "C++ and heap consistent after sync+GC (" +
          std::to_string(checked) + " checked, " +
          std::to_string(mismatches) + " mismatches)");
}

// ===== TEST 6: hiddenRoots page pointers updated after compaction =====

static void testHiddenRootsPointersUpdated(ObjectMemory& memory) {
    std::cout << "\n=== Test: hiddenRoots page pointers valid after GC ===" << std::endl;

    // After GC, all page pointers in hiddenRoots should point to valid objects
    Oop hrOop = memory.hiddenRootsObj();
    CHECK(hrOop.isObject(), "hiddenRootsObj is valid");
    if (!hrOop.isObject()) return;

    ObjectHeader* hr = hrOop.asObjectPtr();
    uint8_t* heapStart = memory.oldSpaceStart();
    uint8_t* heapEnd = memory.oldSpaceFree();

    int validClassPages = 0;
    int validExtraRoots = 0;
    int invalidPointers = 0;
    constexpr size_t MaxPages = 4096;

    for (size_t i = 0; i < MaxPages && i < hr->slotCount(); ++i) {
        Oop pageOop = hr->slotAt(i);
        if (!pageOop.isObject()) continue;
        if (pageOop == memory.nil()) continue;
        if (pageOop.rawBits() == 0) continue;

        auto p = reinterpret_cast<uint8_t*>(pageOop.asObjectPtr());
        if (p >= heapStart && p < heapEnd) {
            ObjectHeader* page = pageOop.asObjectPtr();
            if (page->slotCount() == 1024) {
                validClassPages++;
            } else {
                // Non-1024-slot objects are extra roots (remembered sets, etc.)
                validExtraRoots++;
            }
        } else {
            invalidPointers++;
            if (invalidPointers <= 3) {
                std::cerr << "    Slot " << i << " pointer " << std::hex
                          << reinterpret_cast<uintptr_t>(p) << std::dec
                          << " outside heap" << std::endl;
            }
        }
    }

    CHECK(validClassPages > 0, "Found " + std::to_string(validClassPages) + " class table pages");
    CHECK(invalidPointers == 0, "No dangling pointers in hiddenRoots (" +
          std::to_string(invalidPointers) + " invalid)");
    std::cout << "  Extra root objects: " << validExtraRoots << std::endl;
}

// ===== TEST 7: Image save/reload preserves class table =====

static void testSaveReloadPreservesClassTable(ObjectMemory& memory,
                                               const SpurImageHeader& header) {
    std::cout << "\n=== Test: Save/reload preserves class table ===" << std::endl;

    // Collect class table state before save
    struct ClassEntry { uint32_t index; uint64_t rawBits; };
    std::vector<ClassEntry> entriesBefore;
    for (uint32_t i = 1; i < 8192; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.isObject() && cls != memory.nil()) {
            entriesBefore.push_back({i, cls.rawBits()});
        }
    }
    std::cout << "  Classes before save: " << entriesBefore.size() << std::endl;

    // Sync and GC (same as what primitiveSnapshot does)
    memory.syncClassTableToHeap();
    memory.fullGC();

    // Save image
    std::string tmpPath = "/tmp/test_class_table_roundtrip.image";
    ImageWriter writer;
    SaveResult saveResult = writer.save(tmpPath, memory, header,
                                        memory.lastHash(), 1024, 768);
    CHECK(saveResult.success, "Image saved successfully");
    if (!saveResult.success) {
        std::cerr << "  Save error: " << saveResult.error << std::endl;
        return;
    }

    // Load into a fresh ObjectMemory
    ObjectMemory memory2;
    MemoryConfig config;
    config.oldSpaceSize = 4ULL * 1024 * 1024 * 1024;
    config.newSpaceSize = 32 * 1024 * 1024;
    config.permSpaceSize = 8 * 1024 * 1024;
    CHECK(memory2.initialize(config), "Second memory initialized");

    ImageLoader loader;
    LoadResult loadResult = loader.load(tmpPath, memory2);
    CHECK(loadResult.success, "Image reloaded successfully");
    if (!loadResult.success) {
        std::cerr << "  Load error: " << loadResult.error << std::endl;
        unlink(tmpPath.c_str());
        return;
    }

    // Verify class entries survived
    int lost = 0;
    int found = 0;
    for (auto& entry : entriesBefore) {
        Oop cls = memory2.classAtIndex(entry.index);
        if (cls.isObject() && cls != memory2.nil()) {
            found++;
        } else {
            lost++;
            if (lost <= 5) {
                std::cerr << "    Class at index " << entry.index
                          << " lost after reload" << std::endl;
            }
        }
    }
    CHECK(lost == 0, "All " + std::to_string(entriesBefore.size()) +
          " classes survived reload (" + std::to_string(lost) + " lost)");

    // Verify in-heap pages also consistent in reloaded image
    auto pages2 = getClassTablePages(memory2);
    CHECK(pages2.size() > 0, "Reloaded image has class table pages");

    // Verify reloaded C++ table matches reloaded heap pages
    int mismatches = 0;
    for (size_t i = 1; i < 8192; ++i) {
        Oop cppEntry = memory2.classAtIndex(static_cast<uint32_t>(i));
        Oop heapEntry = readHeapClassEntry(memory2, i);

        if (!cppEntry.isObject() && !heapEntry.isObject()) continue;
        if (cppEntry == memory2.nil() && (heapEntry == memory2.nil() || !heapEntry.isObject())) continue;
        if (!cppEntry.isObject() && heapEntry == memory2.nil()) continue;

        if (cppEntry.rawBits() != heapEntry.rawBits()) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "Reloaded C++ table matches heap pages");

    // Clean up
    unlink(tmpPath.c_str());
    std::string tmpChanges = "/tmp/test_class_table_roundtrip.changes";
    unlink(tmpChanges.c_str());
}

// ===== TEST 8: Multiple GC cycles don't degrade class table =====

static void testMultipleGCCycles(ObjectMemory& memory) {
    std::cout << "\n=== Test: Multiple GC cycles preserve class table ===" << std::endl;

    // Count classes before
    int classesBefore = 0;
    for (uint32_t i = 1; i < 8192; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.isObject() && cls != memory.nil()) classesBefore++;
    }

    // Run 5 GC cycles
    for (int cycle = 0; cycle < 5; ++cycle) {
        memory.syncClassTableToHeap();
        GCResult gc = memory.fullGC();
        (void)gc;
    }

    // Count classes after
    int classesAfter = 0;
    for (uint32_t i = 1; i < 8192; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.isObject() && cls != memory.nil()) classesAfter++;
    }

    CHECK(classesAfter == classesBefore,
          "Same class count after 5 GC cycles (" +
          std::to_string(classesAfter) + " vs " +
          std::to_string(classesBefore) + ")");

    // Verify page integrity
    auto pages = getClassTablePages(memory);
    int badPages = 0;
    for (auto& info : pages) {
        if (info.slotCount != 1024) badPages++;
    }
    CHECK(badPages == 0, "All pages have 1024 slots after 5 GCs");
}

// ===== TEST 9: classTablePages_ vector matches hiddenRoots after GC =====
//
// This is the key invariant that the iPad hang violated: after GC compaction
// moves objects, the C++ classTablePages_ vector must agree with the actual
// hiddenRoots slots. The old bug read hiddenRoots slots during compaction
// when the Oop pointed to the destination but data hadn't moved yet.

static void testClassTablePagesVectorMatchesHiddenRoots(ObjectMemory& memory) {
    std::cout << "\n=== Test: classTablePages_ vector matches hiddenRoots after GC ===" << std::endl;

    // Run GC first to trigger compaction/movement
    GCResult gc = memory.fullGC();
    std::cout << "  GC moved " << gc.objectsMoved << " objects" << std::endl;

    // Now sync so hiddenRoots slots are updated from classTablePages_
    memory.syncClassTableToHeap();

    // Verify: each classTablePages_ entry should match the corresponding
    // hiddenRoots slot.
    Oop hrOop = memory.hiddenRootsObj();
    CHECK(hrOop.isObject(), "hiddenRootsObj valid");
    if (!hrOop.isObject()) return;

    ObjectHeader* hr = hrOop.asObjectPtr();
    const auto& pages = memory.classTablePages();

    int mismatches = 0;
    for (size_t i = 0; i < pages.size() && i < hr->slotCount(); ++i) {
        Oop vectorEntry = pages[i];
        Oop hrSlot = hr->slotAt(i);
        if (vectorEntry.rawBits() != hrSlot.rawBits()) {
            mismatches++;
            if (mismatches <= 3) {
                std::cerr << "    Page " << i
                          << ": vector=0x" << std::hex << vectorEntry.rawBits()
                          << " hiddenRoots=0x" << hrSlot.rawBits() << std::dec << std::endl;
            }
        }
    }
    CHECK(mismatches == 0, "classTablePages_ vector matches hiddenRoots slots (" +
          std::to_string(pages.size()) + " checked, " +
          std::to_string(mismatches) + " mismatches)");
}

// ===== TEST 10: classTablePages_ entries are valid heap pointers after GC =====
//
// The iPad hang produced garbage pointers in the class table page tracking.
// This test verifies every entry in classTablePages_ points to a valid
// in-heap 1024-slot Array object after GC compaction.

static void testClassTablePagesVectorPointersValid(ObjectMemory& memory) {
    std::cout << "\n=== Test: classTablePages_ entries are valid after GC ===" << std::endl;

    // Multiple GC cycles to stress pointer tracking
    for (int i = 0; i < 3; ++i) {
        memory.fullGC();
    }

    const auto& pages = memory.classTablePages();
    uint8_t* heapStart = memory.oldSpaceStart();
    uint8_t* heapEnd = memory.oldSpaceFree();

    int validPages = 0;
    int invalidPages = 0;
    int nilPages = 0;

    for (size_t i = 0; i < pages.size(); ++i) {
        Oop pageOop = pages[i];
        if (!pageOop.isObject() || pageOop.rawBits() == 0 || pageOop == memory.nil()) {
            nilPages++;
            continue;
        }

        auto p = reinterpret_cast<uint8_t*>(pageOop.asObjectPtr());
        if (p >= heapStart && p < heapEnd) {
            ObjectHeader* page = pageOop.asObjectPtr();
            if (page->slotCount() == 1024) {
                validPages++;
            } else {
                invalidPages++;
                std::cerr << "    Page " << i << ": wrong slot count "
                          << page->slotCount() << " (expected 1024)" << std::endl;
            }
        } else {
            invalidPages++;
            std::cerr << "    Page " << i << ": pointer 0x" << std::hex
                      << reinterpret_cast<uintptr_t>(p) << std::dec
                      << " outside heap [0x" << std::hex
                      << reinterpret_cast<uintptr_t>(heapStart) << "-0x"
                      << reinterpret_cast<uintptr_t>(heapEnd) << std::dec << "]" << std::endl;
        }
    }

    CHECK(validPages > 0, "Found " + std::to_string(validPages) + " valid pages in classTablePages_");
    CHECK(invalidPages == 0, "No invalid pointers in classTablePages_ (" +
          std::to_string(invalidPages) + " invalid)");
}

// ===== TEST 11: Force compaction movement, verify class table integrity =====
//
// Allocate garbage objects to fragment the heap, then free them and GC.
// This forces compaction to actually move objects (including class table pages).
// On iPad, this is what triggered the hang — pages moved but the old code
// tried to read their slots from the wrong address.

static void testCompactionMovementPreservesClassTable(ObjectMemory& memory) {
    std::cout << "\n=== Test: Compaction with object movement preserves class table ===" << std::endl;

    // Snapshot class table state before
    int classesBefore = 0;
    std::vector<std::pair<uint32_t, uint64_t>> samplesBefore;
    for (uint32_t i = 1; i < 8192; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.isObject() && cls != memory.nil() && cls.rawBits() != 0) {
            classesBefore++;
            if (samplesBefore.size() < 100) {
                // Store the class's identity hash (stable across moves) rather
                // than raw Oop bits (which change when objects move)
                ObjectHeader* hdr = cls.asObjectPtr();
                samplesBefore.push_back({i, hdr->identityHash()});
            }
        }
    }
    CHECK(classesBefore > 100, std::to_string(classesBefore) + " classes before fragmentation");

    // Allocate many small objects to fragment the heap.
    // Use class index 1 (first real class, usually SmallInteger — but any
    // valid class index works, we just need objects that take space).
    std::vector<Oop> garbage;
    for (int i = 0; i < 500; ++i) {
        Oop obj = memory.allocateSlots(1, 10, ObjectFormat::FixedSize);
        if (obj.isNil()) break;
        garbage.push_back(obj);
    }
    std::cout << "  Allocated " << garbage.size() << " garbage objects" << std::endl;

    // Make half of them unreachable (don't store in any reachable location)
    // by clearing our references. The other half are also unreachable since
    // nothing in the heap points to them — GC will collect all of them.
    garbage.clear();

    // Force full GC with compaction — this MUST move objects
    GCResult gc = memory.fullGC();
    std::cout << "  GC: reclaimed " << gc.bytesReclaimed
              << " bytes, moved " << gc.objectsMoved << " objects" << std::endl;

    // Verify class count unchanged
    int classesAfter = 0;
    for (uint32_t i = 1; i < 8192; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.isObject() && cls != memory.nil() && cls.rawBits() != 0) {
            classesAfter++;
        }
    }
    CHECK(classesAfter == classesBefore,
          "Class count preserved after compaction (" +
          std::to_string(classesAfter) + " vs " +
          std::to_string(classesBefore) + ")");

    // Verify sampled classes still have correct identity hashes
    // (identity hash is stored in the header, survives moves)
    int hashMismatches = 0;
    int classesLost = 0;
    for (auto& [idx, expectedHash] : samplesBefore) {
        Oop cls = memory.classAtIndex(idx);
        if (!cls.isObject() || cls == memory.nil() || cls.rawBits() == 0) {
            classesLost++;
            continue;
        }
        ObjectHeader* hdr = cls.asObjectPtr();
        if (hdr->identityHash() != expectedHash) {
            hashMismatches++;
            if (hashMismatches <= 3) {
                std::cerr << "    Index " << idx << ": hash "
                          << hdr->identityHash() << " != expected " << expectedHash << std::endl;
            }
        }
    }
    CHECK(classesLost == 0, "No classes lost after compaction (" +
          std::to_string(samplesBefore.size()) + " sampled)");
    CHECK(hashMismatches == 0, "All identity hashes preserved after compaction");

    // Verify classTablePages_ vector still valid
    const auto& pages = memory.classTablePages();
    uint8_t* heapStart = memory.oldSpaceStart();
    uint8_t* heapEnd = memory.oldSpaceFree();
    int badPages = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        Oop pageOop = pages[i];
        if (!pageOop.isObject() || pageOop.rawBits() == 0 || pageOop == memory.nil()) continue;
        auto p = reinterpret_cast<uint8_t*>(pageOop.asObjectPtr());
        if (p < heapStart || p >= heapEnd) {
            badPages++;
        } else if (pageOop.asObjectPtr()->slotCount() != 1024) {
            badPages++;
        }
    }
    CHECK(badPages == 0, "All classTablePages_ entries valid after compaction");
}

// ===== TEST 12: New class registration survives GC + save/reload =====
//
// This simulates the user's bug: create a new class in the browser, save,
// reload. The class must appear in the reloaded image's class table.

static void testNewClassSurvivesSaveReload(ObjectMemory& memory,
                                            const SpurImageHeader& header) {
    std::cout << "\n=== Test: New class registration survives GC + save/reload ===" << std::endl;

    // Find any existing class to use as a template
    Oop templateClass;
    for (uint32_t i = 10; i < 200; ++i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.isObject() && cls != memory.nil() && cls.rawBits() != 0) {
            templateClass = cls;
            break;
        }
    }
    CHECK(templateClass.isObject(), "Found template class");
    if (!templateClass.isObject()) return;

    // Register a "new class" at an unused slot (simulates registerClass)
    uint32_t newIndex = 0;
    for (uint32_t i = 800; i > 500; --i) {
        Oop cls = memory.classAtIndex(i);
        if (cls.rawBits() == 0 || cls == memory.nil()) {
            newIndex = i;
            break;
        }
    }
    CHECK(newIndex > 0, "Found unused slot " + std::to_string(newIndex));
    if (newIndex == 0) return;

    memory.setClassAtIndex(newIndex, templateClass);
    uint64_t expectedHash = templateClass.asObjectPtr()->identityHash();

    // Sync to heap + GC (same as primitiveSnapshot does)
    memory.syncClassTableToHeap();
    memory.fullGC();
    memory.syncClassTableToHeap();  // sync again after GC moves things

    // Verify the new class is in both C++ table AND heap page
    Oop cppEntry = memory.classAtIndex(newIndex);
    CHECK(cppEntry.isObject() && cppEntry != memory.nil(),
          "New class in C++ table after sync+GC");

    Oop heapEntry = readHeapClassEntry(memory, newIndex);
    CHECK(heapEntry.isObject() && heapEntry != memory.nil(),
          "New class in heap page after sync+GC");

    // Save and reload
    std::string tmpPath = "/tmp/test_new_class_roundtrip.image";
    ImageWriter writer;
    SaveResult saveResult = writer.save(tmpPath, memory, header,
                                        memory.lastHash(), 1024, 768);
    CHECK(saveResult.success, "Image saved");
    if (!saveResult.success) {
        std::cerr << "  Save error: " << saveResult.error << std::endl;
        memory.setClassAtIndex(newIndex, memory.nil());
        return;
    }

    // Load into fresh memory
    ObjectMemory memory2;
    MemoryConfig config;
    config.oldSpaceSize = 4ULL * 1024 * 1024 * 1024;
    config.newSpaceSize = 32 * 1024 * 1024;
    config.permSpaceSize = 8 * 1024 * 1024;
    CHECK(memory2.initialize(config), "Second memory initialized");

    ImageLoader loader2;
    LoadResult loadResult = loader2.load(tmpPath, memory2);
    CHECK(loadResult.success, "Image reloaded");
    if (!loadResult.success) {
        std::cerr << "  Load error: " << loadResult.error << std::endl;
        unlink(tmpPath.c_str());
        memory.setClassAtIndex(newIndex, memory.nil());
        return;
    }

    // The new class should be at the same index in the reloaded image
    Oop reloaded = memory2.classAtIndex(newIndex);
    CHECK(reloaded.isObject() && reloaded != memory2.nil(),
          "New class survived save/reload at index " + std::to_string(newIndex));

    // Verify identity hash matches
    if (reloaded.isObject() && reloaded != memory2.nil()) {
        CHECK(reloaded.asObjectPtr()->identityHash() == expectedHash,
              "Identity hash preserved through save/reload");
    }

    // Clean up
    unlink(tmpPath.c_str());
    std::string tmpChanges = "/tmp/test_new_class_roundtrip.changes";
    unlink(tmpChanges.c_str());

    // Restore original state
    memory.setClassAtIndex(newIndex, memory.nil());
    memory.syncClassTableToHeap();
}

// ===== TEST 13: classTablePages_ updated through forEachMemoryRoot =====
//
// This directly tests the mechanism that fixed the iPad hang. The old code
// read page pointers from hiddenRoots during compaction (unsafe because the
// object may have moved). The fix uses forEachMemoryRoot to keep classTablePages_
// updated. This test verifies that GC actually updates the vector entries
// when objects move.

static void testClassTablePagesUpdatedByGC(ObjectMemory& memory) {
    std::cout << "\n=== Test: classTablePages_ updated by GC (forEachMemoryRoot) ===" << std::endl;

    // Record page addresses before GC
    const auto& pagesBefore = memory.classTablePages();
    std::vector<uint64_t> rawBitsBefore;
    for (size_t i = 0; i < pagesBefore.size(); ++i) {
        rawBitsBefore.push_back(pagesBefore[i].rawBits());
    }

    // Allocate garbage to increase likelihood of compaction movement
    for (int i = 0; i < 200; ++i) {
        memory.allocateSlots(1, 5, ObjectFormat::FixedSize);
    }

    // Run GC
    GCResult gc = memory.fullGC();
    std::cout << "  GC moved " << gc.objectsMoved << " objects" << std::endl;

    // After GC, the classTablePages_ entries should still be valid.
    // If objects moved, the raw bits may have changed (that's correct —
    // forEachMemoryRoot updates them). What matters is they still point
    // to valid 1024-slot Array objects.
    const auto& pagesAfter = memory.classTablePages();
    CHECK(pagesAfter.size() == rawBitsBefore.size(),
          "classTablePages_ size unchanged (" + std::to_string(pagesAfter.size()) + ")");

    int validAfter = 0;
    int invalidAfter = 0;
    uint8_t* heapStart = memory.oldSpaceStart();
    uint8_t* heapEnd = memory.oldSpaceFree();

    for (size_t i = 0; i < pagesAfter.size(); ++i) {
        Oop pageOop = pagesAfter[i];
        if (!pageOop.isObject() || pageOop.rawBits() == 0 || pageOop == memory.nil()) continue;

        auto p = reinterpret_cast<uint8_t*>(pageOop.asObjectPtr());
        if (p >= heapStart && p < heapEnd) {
            ObjectHeader* page = pageOop.asObjectPtr();
            if (page->slotCount() == 1024) {
                validAfter++;
                // Verify page contents are readable (not garbage)
                // Read first and last slot — if pointer is garbage this will
                // likely segfault or return nonsense
                Oop firstSlot = page->slotAt(0);
                Oop lastSlot = page->slotAt(1023);
                (void)firstSlot;
                (void)lastSlot;
            } else {
                invalidAfter++;
                std::cerr << "    Page " << i << ": slot count " << page->slotCount()
                          << " after GC (expected 1024)" << std::endl;
            }
        } else {
            invalidAfter++;
            std::cerr << "    Page " << i << ": dangling pointer after GC" << std::endl;
        }
    }

    CHECK(validAfter > 0, std::to_string(validAfter) + " valid pages after GC");
    CHECK(invalidAfter == 0, "No corrupted classTablePages_ entries after GC");

    // Also verify that reading class entries through the pages gives
    // the same result as the C++ classTable_ vector
    int entryMismatches = 0;
    for (uint32_t i = 1; i < 4096; ++i) {
        Oop cppEntry = memory.classAtIndex(i);
        Oop heapEntry = readHeapClassEntry(memory, i);
        if (!cppEntry.isObject() && !heapEntry.isObject()) continue;
        if (cppEntry == memory.nil() && (heapEntry == memory.nil() || !heapEntry.isObject())) continue;
        if (!cppEntry.isObject() && heapEntry == memory.nil()) continue;
        if (cppEntry.rawBits() != heapEntry.rawBits()) {
            entryMismatches++;
        }
    }
    // Note: mismatches here are expected if registerClass was called without
    // syncClassTableToHeap. Run sync and re-check.
    memory.syncClassTableToHeap();
    entryMismatches = 0;
    for (uint32_t i = 1; i < 4096; ++i) {
        Oop cppEntry = memory.classAtIndex(i);
        Oop heapEntry = readHeapClassEntry(memory, i);
        if (!cppEntry.isObject() && !heapEntry.isObject()) continue;
        if (cppEntry == memory.nil() && (heapEntry == memory.nil() || !heapEntry.isObject())) continue;
        if (!cppEntry.isObject() && heapEntry == memory.nil()) continue;
        if (cppEntry.rawBits() != heapEntry.rawBits()) {
            entryMismatches++;
        }
    }
    CHECK(entryMismatches == 0,
          "C++ table matches heap pages after GC+sync (" +
          std::to_string(entryMismatches) + " mismatches)");
}

// ===== TEST 14: freeListsObj survives GC =====

static void testFreeListsSurviveGC(ObjectMemory& memory) {  // was Test 9
    std::cout << "\n=== Test: freeListsObj survives GC ===" << std::endl;

    Oop flOop = memory.freeListsObj();
    CHECK(flOop.isObject(), "freeListsObj is valid before GC");

    memory.fullGC();

    Oop flAfter = memory.freeListsObj();
    CHECK(flAfter.isObject(), "freeListsObj is valid after GC");

    // Verify it's still a format-9 object with 64 slots
    if (flAfter.isObject()) {
        ObjectHeader* fl = flAfter.asObjectPtr();
        CHECK(fl->slotCount() == 64, "freeListsObj has 64 slots");
        CHECK(static_cast<uint8_t>(fl->format()) == 9,
              "freeListsObj is format 9 (Indexable64)");
    }
}

// ===== MAIN =====

int main(int argc, char* argv[]) {
    std::cout << "Class Table Integrity Tests" << std::endl;
    std::cout << "===========================" << std::endl;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path-to-Pharo.image>" << std::endl;
        return 1;
    }

    const char* imagePath = argv[1];

    // Initialize memory
    ObjectMemory memory;
    MemoryConfig config;
    config.oldSpaceSize = 4ULL * 1024 * 1024 * 1024;
    config.newSpaceSize = 32 * 1024 * 1024;
    config.permSpaceSize = 8 * 1024 * 1024;

    if (!memory.initialize(config)) {
        std::cerr << "ERROR: Failed to initialize memory" << std::endl;
        return 1;
    }

    // Load image
    ImageLoader loader;
    LoadResult result = loader.load(imagePath, memory);
    if (!result.success) {
        std::cerr << "ERROR: Failed to load image: " << result.error << std::endl;
        return 1;
    }
    std::cout << "Image loaded: " << result.heapSize / (1024*1024) << " MB" << std::endl;

    // Run tests
    testClassTablePagesExist(memory);
    testClassTableConsistency(memory);
    testClassTableSurvivesGC(memory);
    testSyncClassTableToHeap(memory);
    testClassTableSurvivesGCWithSync(memory);
    testHiddenRootsPointersUpdated(memory);
    testSaveReloadPreservesClassTable(memory, loader.header());
    testMultipleGCCycles(memory);
    testClassTablePagesVectorMatchesHiddenRoots(memory);
    testClassTablePagesVectorPointersValid(memory);
    testCompactionMovementPreservesClassTable(memory);
    testNewClassSurvivesSaveReload(memory, loader.header());
    testClassTablePagesUpdatedByGC(memory);
    testFreeListsSurviveGC(memory);

    // Summary
    std::cout << "\n===========================" << std::endl;
    std::cout << "Results: " << passes << " passed, " << failures << " failed" << std::endl;

    if (failures > 0) {
        std::cerr << "\nFAILED: " << failures << " test(s) failed!" << std::endl;
        return 1;
    }

    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
