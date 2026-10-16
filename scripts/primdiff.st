"primdiff.st -- differential check of pure primitives against Cog.
Calls each pure primitive (arithmetic, comparison, at:, float, character,
identity) of 20 core classes with 34 edge-case receivers and arguments and
writes one line per call to the file named by the global PRIMDIFFOUT (default
/tmp/primdiff-out.txt).  Run it on both VMs and diff the two files.  See
docs/vm-compat-bugs.md, 2026-10-17.  Under Cog, leave ByteString and
ByteSymbol out of the class list: Cog crashes on compareWith: 1."

| vals pure classes out f n |
vals := { 0. 1. -1. 7. -7. 3. SmallInteger maxVal. SmallInteger minVal. 1 bitShift: 62. (1 bitShift: 62) negated. 1 bitShift: 100. (1 bitShift: 100) negated. 12345678901234567890. 3.5. -0.0. 0.0. 1.0e308. 2.5e-310. Float nan. Float infinity. Float negativeInfinity. 1/3. $a. (Character value: 300). 'abc'. 'ABD'. ''. #abc. #(1 2 3). #(). nil. true. #[1 2 255]. 1.5 }.
pure := (1 to: 17) asSet, (18 to: 59) asSet, #(60 62 63 110 111 158 159 165 166 169 170 171 541 542 543 544 545 546 547 548 549 550 551 552 553 554 555 556 557 558 559 575) asSet.
classes := { SmallInteger. LargePositiveInteger. LargeNegativeInteger. Integer. Number. Float. BoxedFloat64. SmallFloat64. Fraction. Character. String. ByteString. Symbol. ByteSymbol. Array. ArrayedCollection. ByteArray. Object. ProtoObject. Magnitude }.
out := WriteStream on: (String new: 100000).
n := 0.
classes do: [:c |
  (c selectors asSortedCollection: [:a :b | a <= b]) do: [:sel | | m |
    m := c compiledMethodAt: sel.
    ((pure includes: m primitive) and: [ m numArgs <= 1 ]) ifTrue: [
      vals doWithIndex: [:r :ri |
        (r isKindOf: c) ifTrue: [
          (m numArgs = 0 ifTrue: [ #(0) ] ifFalse: [ 1 to: vals size ]) do: [:ai | | res |
            res := [ | v | v := m numArgs = 0
                        ifTrue: [ r perform: sel ]
                        ifFalse: [ r perform: sel with: (vals at: ai) copy ].
                     v isFloat ifTrue: [ v isNaN ifTrue: ['NaN'] ifFalse: [ v printString, ' ', (v asIEEE64BitWord printStringBase: 16) ] ]
                       ifFalse: [ v printString ] ]
              on: Error, ZeroDivide do: [:e | 'ERR ', e class name ].
            n := n + 1.
            out nextPutAll: c name; nextPutAll: '>>'; nextPutAll: sel; space; print: ri; space; print: ai; space;
              nextPutAll: (res size > 80 ifTrue: [ res first: 80 ] ifFalse: [ res ]); lf ] ] ] ] ] ].
f := (Smalltalk at: #PRIMDIFFOUT ifAbsent: [ '/tmp/primdiff-out.txt' ]) asFileReference.
f ensureDelete. f writeStreamDo: [:s | s nextPutAll: out contents ].
n
