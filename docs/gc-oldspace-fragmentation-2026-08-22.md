# Old space is 2.7x bigger than the objects in it, and nine pinned objects are why

Measured 2026-08-22 at `02195e8f`, arm64, on the image
`scripts/package-tests-selfhosted.sh` produces after loading NeoJSON.

## The number

    image file on disk                       247 MB
    old space in use (oldSpaceFree - start)  235 MB
    live objects actually in it               89 MB     <-- 758,832 objects

Both halves are measured, not inferred, and they agree with each other from
two independent directions:

  * From inside the image, after `Smalltalk garbageCollect`:
    `SystemNavigation default allObjectsDo: [ :o | tot := tot + o sizeInMemory ]`
    answers 91,005,736 bytes over 759,583 objects.
  * From the VM, walking old space at the end of `fullGC`: 758,832 objects
    totalling 93,148,520 bytes.

The same measurement on the unmodified base image agrees with itself:
56,001,824 bytes live against a 55 MB old space and a 52 MB file. So the
instruments are fine, and the base image is fine. It is the loaded image
that carries 146 MB of nothing.

## Why compaction does not reclaim it

`PHARO_GC_LOG=1` now prints the compaction plan:

    [GC-PLAN] seen=758832 marked=758832 unmarked=0 pinned=9
              inplace=758823 tomove=0 markedBytes=93148520
              toFinger=+241075 KB src=[+0,+241075] KB
    [GC-LOG] fullGC #1 done: used=235 MB (objects=88 MB in 758832) reclaimed=0 MB
    [GC-LOG]   passes=1 moved=0
    [GC-LOG]   pinned @+559 KB    size=8208     cls=19 (ByteArray)
    [GC-LOG]   pinned @+567 KB    size=8208     cls=19
    [GC-LOG]   pinned @+12690 KB  size=2097168  cls=19
    [GC-LOG]   pinned @+166792 KB size=32       cls=50
    [GC-LOG]   pinned @+167539 KB size=32       cls=50
    [GC-LOG]   pinned @+182912 KB size=32       cls=50
    [GC-LOG]   pinned @+192166 KB size=32       cls=50
    [GC-LOG]   pinned @+202190 KB size=32       cls=50
    [GC-LOG]   pinned @+208455 KB size=32       cls=50
    [GC-LOG]   lastObjectEnd=+241075 KB  oldSpaceFree=+241075 KB

Read the plan line: every object is marked (nothing in this heap is dead),
**every non-pinned object is already in place, and nothing moves** — yet the
destination finger travels 235 MB to lay out 89 MB of objects.

The whole difference comes from one branch in
`planCompactSavingForwarders`:

    if (obj->isPinned()) {
        if (toFinger < objStart) toFinger = objStart;   // skip the gap
        ...
    }

A pinned object cannot move, so the finger jumps forward to it and the
space below is abandoned. Six 32-byte pinned objects sitting between
+166 MB and +208 MB are enough to strand 146 MB. Everything after each jump
is then "already in place", which is why `moved=0`.

And it is self-perpetuating. The compaction that ran when the image was
SAVED slid the live set down, left the vacated span zeroed, and could not
lower `oldSpaceFree_` past the pins — so the image writer, which writes
`oldSpaceStart .. oldSpaceFree` verbatim, wrote 146 MB of zeros into the
file. Loading it back reproduces the layout exactly, every object is in
place again, and no later GC can improve it.

`ObjectScanner` skips zero words as padding, which is why the gaps do not
show up as free chunks in any walk — the census reports "free-chunks=0 MB"
while 146 MB of the span is exactly that.

## It is much worse than 2.7x on a big load

The XMLParser package image, same script, same day:

    image file on disk                     1,202,629,400 bytes   (1.15 GB)
    live objects (allObjectsDo/sizeInMemory)   98,296,808 bytes   (94 MB)
                                               837,995 objects

**12x.** A 52 MB base image plus one package baseline produces a 1.15 GB
file holding 94 MB of objects, and every launch of that image reads and
faults in all 1.15 GB. The package tier pays this twice per package (the
load writes it, the test pass reads it).

## Consequences that are already being paid

  * Package images are ~2.7x their content: 235 MB (NeoJSON), 163 MB
    (Mustache), and the 2026-08-17 run recorded 401 MB (Grease), 559 MB
    (DataFrame), 1056 MB (XMLParser). Every subsequent launch reads all of
    it off disk and faults it in.
  * `rebuildFreeListAfterCompact()` does not put those gaps on a free list —
    its own comment says "Free lists will be populated when we switch to
    free-list-based allocation" — so allocation cannot reuse them either.
    The heap only ever grows past them.

## The three candidate fixes, and what each buys

1. **Populate the free list with the post-compaction gaps** and allocate
   from it. Stops the growth (the 146 MB becomes usable) but does not shrink
   the file, since `oldSpaceFree_` still sits above the top pin. This is the
   fix the existing comment promises.
2. **Place pinned objects low.** Spur's `pinObject` copies the object and
   `become:`s it rather than pinning it wherever it happens to sit; nothing
   holds the address yet at pin time, which is the whole point of pinning it.
   Doing the same here keeps the pins out of the way and lets compaction
   reach `oldSpaceFree_` down to the live size — which fixes the file size
   too. Only helps pins created after the change.
3. **Compact into the gaps** (free-list-aware / two-finger compaction).
   Largest change, fixes both, and subsumes (1).

Not yet decided. What is decided is that this is not "Metacello allocates a
lot": 89 MB of it is real and 146 MB of it is zeros.

## (1) was attempted, and it is NOT ready — `PHARO_OLDSPACE_FREELIST`

The knob exists and is OFF by default. Two halves, one of which works:

  * **The gap accounting works.** `rebuildFreeListAfterCompact` walks old
    space, finds each span between consecutive objects, and puts it on the
    free list. On the NeoJSON image: `[GC-FREELIST] 17 gaps, 148 MB
    reclaimed for reuse (used=235 MB)`, and the post-GC census then reports
    them honestly as `free-chunks=146 MB in 10` where it used to say 0 —
    because `ObjectScanner` skips zero words, so before this the gaps were
    invisible to every walk in the VM.
  * **Allocating from them is not safe yet.** With the knob on, a NeoJSON
    Metacello load STALLS: `[PROGRESS] 20s: ~93575837 steps` followed by
    `[PROGRESS] 30s: ~93575837 steps` — the same count, i.e. no bytecodes
    executed for ten seconds.

One real bug was found and fixed on the way: `makeFreeChunk` memset the
whole chunk body, and `allocateFromFreeList` calls it on the REMAINDER of
every split. With one 148 MB gap on the list that is a 148 MB memset per
allocation. `makeFreeChunk` now takes `zeroBody` and the split path passes
false. That took the load from "5x slower and climbing" to "stalls", so it
was necessary but not sufficient.

### Found by inspection afterwards: `makeFreeChunk` mis-sizes every LARGE chunk

    ObjectHeader* chunk = (ObjectHeader*)addr;
    size_t slotCount = (size - sizeof(ObjectHeader)) / 8;      // (size - 8) / 8
    ...
    if (slotCount >= 255) {
        uint64_t* overflow = (uint64_t*)addr;
        *overflow = slotCount | (0xFFULL << 56);               // word 0
        chunk = (ObjectHeader*)(addr + 8);                     // header at word 1
        chunk->setRawHeader(makeHeader(255, ...));
    }

For a chunk of 255 slots or more the header needs TWO words — the overflow
count and the header itself — but `slotCount` was computed for one. The
object placed at `addr + 8` therefore claims `8 + slotCount*8` bytes starting
one word in, i.e. it runs **8 bytes past the end of the chunk it describes**.

That alone is a latent defect wherever large free chunks are built at all,
which includes `sweepGC`, not just the new opt-in path.

It also explains the stall directly. `allocateFromFreeList` then does

    remainderAddr = (uint8_t*)chunk + size;

with `chunk` already being `addr + 8`, so every split places the remainder
one word past where it belongs, and each subsequent split compounds it until
the heap no longer parses — which is what a VM that stops executing
bytecodes looks like from outside.

Two more, smaller, in the same function:

  * a chunk is accepted when `chunkSize >= size`, and if the leftover is
    under 16 bytes it is neither split off nor zeroed — the caller writes a
    header claiming `size` and the scanner walks into the orphaned tail. The
    fix is to require `chunkSize == size || chunkSize - size >= 16`.
  * the large-list walk stores a raw `ObjectHeader**` into a slot that
    otherwise holds an `Oop`
    (`prev = reinterpret_cast<ObjectHeader**>(&chunk->slots()[0])`). Benign
    only because a heap `Oop` and its address have the same bits and `nil`
    is 0 — worth making explicit rather than relying on it.

### The sizing bug IS fixed — and the stall survives it

All three were corrected (`makeFreeChunk` takes the two header words into
account, `allocateFromFreeList` measures the remainder from the chunk's true
start, and a chunk is only accepted on an exact fit or a >=16-byte
leftover). Then measured:

    SUnit batch 1-50, knob OFF   774 tests, 772 P, 0 F, 0 E
                                 byte-identical to the day's baseline
    NeoJSON load, knob ON        still stalls: 80 s elapsed, ~17,520
                                 bytecodes executed (219 steps/s)

So the mis-sizing was real and worth fixing on its own — `sweepGC` builds
large free chunks through that function and has been describing them 8 bytes
too long — but it is **not** what stalls the allocator. Something else in
that path is still wrong, and the knob stays OFF.

Whoever picks this up: the remaining suspect list is empty, which means the
next step is instrumentation rather than reading. Log every
`allocateFromFreeList` hit (chunk address, chunk size, request, remainder)
for the first few hundred allocations and find the first one whose result
cannot be re-parsed by `ObjectScanner`.

## Reproducing

    PHARO_GC_LOG=1 build-rel/test_load_image <pkg.image> eval \
        "Smalltalk garbageCollect. 'ok'"

and, from inside any image,

    | tot n | tot := 0. n := 0.
    Smalltalk garbageCollect.
    SystemNavigation default allObjectsDo: [ :o | tot := tot + o sizeInMemory. n := n + 1 ].
    'liveBytes=', tot printString, ' objects=', n printString

## 2026-08-23: the pins identified, and why candidate 2 is the fix

Enumerated from inside the loaded NeoJSON package image (245 MB file) after a
full GC, using `allObjectsDo:` + `isPinned`:

    PIN count=6
    PIN 6 x ByteArray sz=32

**Six 32-byte ByteArrays strand 146 MB.** Their size and class say FFI:
`ByteArray>>pinInMemory` is how Pharo holds a callout parameter buffer still
while C looks at it.

And our pin never relocates. `Interpreter::primitivePin`
(`src/vm/Primitives.cpp:11199`) is, in full, follow-forwarders then

    ObjectHeader* header = rcvr.asObjectPtr();
    bool wasPinned = header->isPinned();
    header->setPinned(shouldPin);

i.e. it sets the bit **wherever the object already sits**. If that address is
high in old space, `planCompactSavingForwarders` must jump the destination
finger up to it and abandon everything below — which is exactly the
`inplace=758823 tomove=0 pinned=9` plan recorded above.

Spur does not do this: `pinObject` COPIES the object to a low address and
`become:`s it, precisely because at pin time nothing holds the address yet —
taking the address is what the caller is about to do. That is candidate 2, and
this measurement is what makes it the right one: the objects being pinned are
tiny and few, so relocating them is cheap, and they are the entire cause.

### Specification for whoever implements it

In `primitivePin`, when `shouldPin && !wasPinned` and the receiver lives in old
space above the live set:

  1. allocate a copy low (free list, or a small dedicated pinned region near
     `oldSpaceStart`),
  2. copy the header and body,
  3. mark the COPY pinned,
  4. install a forwarding pointer from the original to the copy so existing
     references follow (`followForwarded` already exists and `primitivePin`
     already calls it on entry).

Do NOT try to move pinned objects during compaction instead — by then the
caller may hold the address, which is the whole reason the object is pinned.
Relocation is only safe at pin time.

Validate against the numbers already in this file: the NeoJSON image should
fall from 245 MB toward ~90 MB, and XMLParser from 1.15 GB toward ~100 MB.
Treat GC changes carefully — an eviction change in this project cost a 7x
regression once — so run both test tiers before and after.

### Two facts measured 2026-08-23 that shape the implementation

**1. The call rate is affordable.** `PHARO_PIN_STATS=1` over a complete NeoJSON
package load:

    [PIN-STATS] primitivePin calls=638 newlyPinned=625

`becomeForward` is an EAGER full-heap walk (`ObjectMemory.cpp:1604` —
`allObjectsDo` replacing references), so the cost is one walk per relocation.
625 worst case is affordable for a load that already takes ~30 s, and it drops
much further once relocation is restricted to receivers **already in old
space**: fresh callout buffers are young, and only 6 pins remain at rest.

**2. Candidate 2 DEPENDS ON candidate 1.** Relocating low needs somewhere low
to put the object, and there is no working low allocator today.
`allocateFromFreeList` exists but the post-compaction gaps are only put on the
free list under `PHARO_OLDSPACE_FREELIST`, which `debug_vars.h` itself marks
EXPERIMENTAL AND CURRENTLY BROKEN (it stalls a load at ~17,520 bytecodes).

So the order of work is: **fix the free list first** (candidate 1, which the
existing `rebuildFreeListAfterCompact` comment already promises), then
relocation-on-pin becomes a small change on top of it. Attempting candidate 2
alone means inventing a second low-address allocator — a dedicated pinned
region reserved near `oldSpaceStart` — which is more invasive than fixing the
allocator that is already half-written.


## 2026-08-23: candidate 1 is DONE — the free list works, so candidate 2 is unblocked

`PHARO_OLDSPACE_FREELIST` now functions. Six bugs had to be fixed; the last was
the one that mattered:

    collectInstancesOfClass matched classIndex 0, and indexOfClass answers 0 as
    its NOT-FOUND sentinel, so `allInstances` of a class with no class-table
    entry returned an Array of FREE CHUNKS. Pharo's class-shape migration asks
    exactly that, and died on the first chunk.

Measured with the knob on:

    300-class SUnit batch   150s  4861 pass  0 fail  0 error  1 timeout
    same, knob off          139s  4861 pass  0 fail  0 error  1 timeout
    NeoJSON package load    20s   rc=0
    VM binaries             pass both ways

So there is now a working low allocator, which is what candidate 2
(relocate pinned objects at pin time, Spur-style) was blocked on. The
implementation spec above stands; the affordability measurement (638 pin calls
per package load) stands; and the dependency noted there is now satisfied.

The knob remains DEFAULT-OFF pending a broader soak — a full SUnit sweep and
the package tier on both arches — because this file's own history is a warning
about shipping GC changes on partial evidence.

### Soak results for the working free list (2026-08-23)

    VM binaries      sista_ir, class_table, relaunch 3/3 with the knob ON;
                     class_table + relaunch 9/9 cycles with it OFF
    SUnit 300 classes  knob ON  150s  4861 pass  0 fail  0 error  1 timeout
                       knob OFF 139s  4861 pass  0 fail  0 error  1 timeout
    package tier     NeoJSON 116 pass / Mustache 47 / Grease 554, ALL matching
                     the recorded baselines exactly, 0 fail 0 error throughout
                     (Grease loads in 36s against 71s at baseline)

Image sizes are unchanged (245/173/450 MB against 245/172/432 MB), which is
expected: reusing the gaps stops the heap GROWING past them but cannot lower
`oldSpaceFree_` below the top pin. Shrinking the file is what candidate 2
buys, and candidate 2 is what the free list was needed for.

### A "silent no-op" that is NOT a bug

While hunting the above it looked as though class-shape migration might do
nothing on the default path, because `allInstances` answers `#()` for a class
with no class-table entry. Checked directly:

    MIG allInstancesBefore=1 instSizeBefore=0 instSizeAfter=1 instClass=ZZMig

Instantiating a class registers it, so a class that HAS instances is found and
its instances are migrated. `#()` only comes back for classes with no
instances, where it is the correct answer. No default-path defect, and the
classIndex-0 guard masks nothing.


## 2026-08-23: candidate 2 implemented — and measured to hook the WRONG event

`ObjectMemory::relocateToLowSpace` + `PHARO_PIN_RELOCATE` implement the spec
above: at pin time, copy the object into a reclaimed low gap, carry the mark
bit, re-apply the old->young barrier, `becomeForward` the original, and only
ever move DOWNWARD. It is correct and harmless (NeoJSON load rc=0 with it on),
but on a real package load it **never fires**:

    primitivePin calls=638 newlyPinned=625 relocatedLow=0
    skip[ notObj=0  young=439  pinned=0  noChunk=186  notLower=0 ]

439 + 186 = 625, i.e. every single one was skipped, for two reasons:

  * **young=439 (70%)** — the object is still in EDEN at pin time.
    `ByteArray>>pinInMemory` is called on a buffer immediately after
    allocating it, long before any tenuring. Relocating "low in old space"
    is meaningless then.
  * **noChunk=186 (30%)** — genuinely in old space, but the free list had no
    chunk to give. The gaps only appear after a fullGC has run and rebuilt the
    list, so early in a load there is nothing to move into.

**So pin time is the wrong hook.** The pins that end up stranding old space are
young objects that get TENURED while pinned, and tenuring is what decides where
they land. The corrected design is:

  1. At tenure/promotion, if the object being promoted is PINNED, allocate its
     old-space home from the free list (low) rather than by bumping
     `oldSpaceFree_`. That is the event that actually places these six
     32-byte buffers.
  2. Keep pin-time relocation for the old-space minority, but only once a
     fullGC has populated the list — it is a no-op before then.

The code is left in place, default-off, because it is correct as far as it
goes and step 2 still wants it. Do not delete it; re-point step 1 at tenuring.


### And tenure time does not work either — the low space does not exist yet

Moved the placement to `tenureIfYoung` (the event the skip census pointed at):
a pinned object being promoted asks `allocateFromFreeList` for its old-space
home instead of bumping `oldSpaceFree_`. Measured on the same load:

    base    rc=0  29s  image=240MB  tenuredLow=0
    reloc   rc=0  19s  image=237MB  tenuredLow=0

**Zero.** Same underlying reason as the earlier `noChunk=186`: the free list is
built by `rebuildFreeListAfterCompact`, which only runs after a fullGC has
compacted. During a package load the pins are placed long before that, so at
the moment a low home is wanted there are no gaps on the list at all.

This is chicken-and-egg, and it rules out BOTH hooks as specified:

    pin time     70% of pins are still in eden      -> nothing to relocate
    tenure time  free list is empty that early      -> nowhere low to go

So candidate 2 cannot be built on the free list alone. It needs the option this
file originally called "more invasive": a small region reserved for pinned
objects near `oldSpaceStart_` at image load, independent of whether any GC has
run. That reservation is the actual prerequisite; the relocation and tenure
code (both default-off, both correct, both measured harmless) are ready to use
it the moment a low home exists.


### The pin arena: first measurable win, and its limit

Carve a 256 KB arena the first time a pin needs an old-space home
(`ObjectMemory::allocatePinnedLow`) and tenure pinned objects into it. Unlike
the free list it exists during a load, which was the blocker.

    base (no arena)   image=240MB   tenuredLow=0
    arena             image=226MB   tenuredLow=2
    arena, rerun      image=234MB   tenuredLow=2

So it works and it is the first change to move the number — but only two pins
are captured, and the resting pinned count is six. The saving (~6-14 MB of
240 MB) is a fraction of the 146 MB the analysis says is stranded, because the
other four pins reach old space by some route other than scavenge tenuring and
are still placed wherever they land.

**Do NOT feed pin-time relocation from the arena.** Tried: the VM crashes in
3 s (rc=133) on a NeoJSON load. `relocateToLowSpace` runs inside `primitivePin`
and ends in `becomeForward`, a full-heap walk that rewrites references while
the interpreter still holds oops for the primitive in flight. The tenure path
is safe precisely because it runs inside scavenge, where moving objects is
already the contract. That asymmetry is the reason the two hooks behave
differently and is worth remembering.

Next, for whoever continues: find how the other four pins arrive in old space
(they are not tenured by scavenge — instrument `allocateRaw`'s Old case for
pinned receivers) and route those through the arena too. Everything else is in
place: the arena, the free list, and the measured costs.

Validated: VM binaries pass with `PHARO_PIN_RELOCATE=1` (sista_ir,
class_table, relaunch 3/3) and with it off (relaunch 3/3).


### Arena placement was wrong, and the pins that matter never reach it

Carving the arena lazily on first demand put it **at +199,499 KB** on a NeoJSON
load -- ~195 MB in, i.e. ABOVE most of what it was supposed to sit below. That
is why the first version saved only ~6%. Carving it in
`setOldSpaceFreePointer` instead, the moment the image is in place, puts it
where it belongs:

    PIN-ARENA carved 256 KB at +53276 KB     (just past the loaded image)

But the pin census at the final GC shows the arena is still not catching the
objects that matter:

    pinned @+559 KB    size=8208     (from the image itself)
    pinned @+567 KB    size=8208
    pinned @+12690 KB  size=2097168
    pinned @+183164 KB size=32       <-- these four are the 146 MB
    pinned @+183917 KB size=32
    pinned @+211291 KB size=32
    pinned @+226003 KB size=32

None of them is in the arena at +53276 KB, and `tenuredLow=2` accounts for two
different objects that did not survive. **So the four 32-byte pins that strand
old space never pass through scavenge tenuring at all.**

That is the next question, and it is now sharply posed: how does a 32-byte
pinned ByteArray get into old space at +183..+226 MB without being tenured by
`tenureIfYoung`? Instrument `allocateRaw`'s Old case (and any other old-space
allocation path) to log allocations that are, or shortly become, pinned. Once
that route is known, point it at `allocatePinnedLow` exactly as tenuring now
is, and the arena will finally hold all of them.

Validated at this step: `class_table` ALL TESTS PASSED and `test_relaunch` 3/3
with `PHARO_PIN_RELOCATE=1`, and 3/3 with it off.
