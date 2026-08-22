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

## Reproducing

    PHARO_GC_LOG=1 build-rel/test_load_image <pkg.image> eval \
        "Smalltalk garbageCollect. 'ok'"

and, from inside any image,

    | tot n | tot := 0. n := 0.
    Smalltalk garbageCollect.
    SystemNavigation default allObjectsDo: [ :o | tot := tot + o sizeInMemory. n := n + 1 ].
    'liveBytes=', tot printString, ' objects=', n printString
