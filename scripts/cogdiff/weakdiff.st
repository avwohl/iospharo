"weakdiff.st -- weak references, ephemerons and finalization after a GC, for a diff against
Cog.  2026-10-17: all 14 agree."

| results run |
results := OrderedCollection new.
run := [:name :blk | results add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name ]) ].
run value: #weakArray value: [ | w | w := WeakArray new: 3. w at: 1 put: Object new; at: 2 put: #sym; at: 3 put: 'lit'. Smalltalk garbageCollect. { w at: 1. w at: 2. w at: 3 } ].
run value: #weakArrayKept value: [ | w o | o := Object new. w := WeakArray with: o. Smalltalk garbageCollect. (w at: 1) == o ].
run value: #weakKeyDict value: [ | d k | d := WeakKeyDictionary new. k := Object new. d at: k put: 1. d at: Object new put: 2. Smalltalk garbageCollect. d finalizeValues. { d size. d at: k } ].
run value: #weakValueDict value: [ | d v | d := WeakValueDictionary new. v := Object new. d at: #a put: v. d at: #b put: Object new. Smalltalk garbageCollect. { (d at: #a) == v. d at: #b ifAbsent: [#gone] } ].
run value: #weakSet value: [ | s o | s := WeakSet new. o := Object new. s add: o; add: Object new. Smalltalk garbageCollect. { s size. s includes: o } ].
run value: #finalization value: [ | sem o hit | sem := Semaphore new. hit := false. o := Object new. o finalizationRegistry add: o executor: (ObjectFinalizer receiver: [ hit := true. sem signal ] selector: #value). o := nil. Smalltalk garbageCollect. sem waitTimeoutMilliseconds: 2000. hit ].
run value: #ephemeronMourn value: [ | reg o got sem | sem := Semaphore new. got := nil. reg := FinalizationRegistry new. o := 'payload' copy. reg add: o finalizer: (ObjectFinalizer receiver: [ got := #mourned. sem signal ] selector: #value). o := nil. Smalltalk garbageCollect. sem waitTimeoutMilliseconds: 2000. got ].
run value: #gcResult value: [ Smalltalk garbageCollect > 0 ].
run value: #gcMost value: [ Smalltalk garbageCollectMost > 0 ].
run value: #weakAfterBecome value: [ | w a b | a := Object new. b := Object new. w := WeakArray with: a. a become: b. Smalltalk garbageCollect. (w at: 1) == a ].
run value: #weakRegistry value: [ | r o | r := (Smalltalk at: #FinalizationRegistry) new. o := Object new. r add: o. Smalltalk garbageCollect. r size ].
run value: #weakAnnouncer value: [ | a count sub | a := Announcer new. count := 0. sub := Object new. a weak when: Announcement send: #yourself to: sub. sub := nil. Smalltalk garbageCollect. a numberOfSubscriptions ].
run value: #allInstancesAfterGC value: [ | n | Smalltalk garbageCollect. n := (Object subclasses select: [:c | c name = #Point]) size. n ].
run value: #weakLiteral value: [ | w | w := WeakArray with: 3 with: $a with: 1.5. Smalltalk garbageCollect. w asArray ].
results asArray
