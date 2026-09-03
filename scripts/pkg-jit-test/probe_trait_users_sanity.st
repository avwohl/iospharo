"Probe for open defect #23 (the trait storm) via defect-23's cheapest handle.

 TraitTest>>testTraitsUsersSanity asserts a whole-image invariant:

    every class is listed by the traits it uses, and
    every trait's users still use it

 It fails for us on x86_64 (53 P / 1 F where Cog is 54 P / 0 F) and it runs
 late in the trait block, after two dozen classes have created and rebuilt
 traits through ShiftClassBuilder.  So the interesting question is not that it
 fails but WHICH entries are stale -- that names what a rebuild leaves behind.

 This runs the trait block itself and then reports the offenders, rather than
 just asserting.  Writes /tmp/trait_users_sanity.txt.

 Run: build-rel/test_load_image <image> eval
        ""'<this file>' asFileReference fileIn""
 Also useful BEFORE running anything, to confirm a pristine image is clean."
| out check classes path flush |
out := WriteStream on: String new.
path := '/tmp/trait_users_sanity.txt'.
path asFileReference ensureDelete.
"Flush after every class.  If the run storms -- which is the whole point of
 pointing this at our VM -- the process dies and a report written only at the
 end would be lost, taking the one thing we wanted with it."
flush := [
    path asFileReference ensureCreateFile.
    path asFileReference writeStreamDo: [ :s | s nextPutAll: out contents ] ].
check := [ :label |
    | badClasses badTraits |
    badClasses := OrderedCollection new.
    badTraits := OrderedCollection new.
    [ Smalltalk allClassesAndTraits do: [ :each |
        each traits do: [ :t |
            (t traitUsers includes: each) ifFalse: [
                badClasses add: each name , ' uses ' , t name , ' but is not in its traitUsers' ] ] ] ]
      on: Error do: [ :e | badClasses add: 'WALK ERROR: ' , e class name ].
    [ Smalltalk globals allTraits do: [ :each |
        each traitUsers do: [ :b |
            (b traits includes: each) ifFalse: [
                badTraits add: each name , ' lists ' , b printString , ' as a user, but it does not use the trait' ] ] ] ]
      on: Error do: [ :e | badTraits add: 'WALK ERROR: ' , e class name ].
    out nextPutAll: '--- '; nextPutAll: label; lf.
    out nextPutAll: '    class->trait violations : '; nextPutAll: badClasses size printString; lf.
    out nextPutAll: '    trait->user  violations : '; nextPutAll: badTraits size printString; lf.
    (badClasses first: (badClasses size min: 20)) do: [ :l |
        out nextPutAll: '      '; nextPutAll: l; lf ].
    (badTraits first: (badTraits size min: 20)) do: [ :l |
        out nextPutAll: '      '; nextPutAll: l; lf ] ].

check value: 'before running anything'.
flush value.

classes := (TestCase allSubclasses
    select: [ :c | (c name beginsWith: 'Trait')
                     and: [ ([ c isAbstract ] on: Error do: [ :e | true ]) not ] ])
    asSortedCollection: [ :a :b | a name <= b name ].
out nextPutAll: 'running '; nextPutAll: classes size printString; nextPutAll: ' Trait*Test classes'; lf.
classes do: [ :c |
    | r |
    r := [ c suite run ] on: Error do: [ :e | nil ].
    out nextPutAll: '    '; nextPutAll: c name;
        nextPutAll: (r isNil
            ifTrue: [ '  RUN-ERROR' ]
            ifFalse: [ '  P:' , r expectedPasses size printString ,
                       ' F:' , r failures size printString ,
                       ' E:' , r errors size printString ]); lf.
    check value: 'after ' , c name.
    flush value ].

out nextPutAll: 'DONE'; lf.
flush value.
