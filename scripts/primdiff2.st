"primdiff2.st -- like primdiff.st, for primitives with up to four arguments
(at:put:, replaceFrom:to:with:startingAt:, LargeIntegers and MiscPrimitivePlugin
named primitives, class-side string primitives), on copies of the receivers.
Answers and the receiver after the call are recorded.  2026-10-17: 124
methods, 40819 calls, all equal to Cog.  Sort both outputs before diffing
when the Cog side is run one class at a time."

| vals small okMethod classes out f n show |
vals := { 0. 1. -1. 7. 3. 255. 256. SmallInteger maxVal. SmallInteger minVal. 1 bitShift: 62. (1 bitShift: 62) negated. 1 bitShift: 100. (1 bitShift: 100) negated. 12345678901234567890. 3.5. -0.0. 1.0e308. Float nan. Float infinity. 1/3. $a. (Character value: 300). 'abc'. 'hello world'. ''. #abc. #(1 2 3). nil. true. #[1 2 255]. (ByteArray new: 300). 'abc' asWideString. (1 to: 5) asArray, #(nil). 1.5 }.
small := { 0. 1. 2. 3. -1. 255. 1 bitShift: 62. 3.5. $a. 'ab'. #[1 2]. nil }.
okMethod := [:m | | p | p := m primitive.
   (((p between: 1 and: 63) or: [ #(64 65 66 105 110 111 158 159 165 166 169 170 171) includes: p ]) or: [ p > 540 and: [ p < 576 ] ])
     or: [ p = 117 and: [ #('LargeIntegers' 'MiscPrimitivePlugin') includes: (m literalAt: 1) first ] ] ].
classes := { SmallInteger. LargePositiveInteger. LargeNegativeInteger. Integer. Number. Float. BoxedFloat64. SmallFloat64. Character. String. ByteString. WideString. Symbol. Array. ArrayedCollection. ByteArray. Object. ProtoObject. SequenceableCollection. ByteString class. String class. ByteArray class. LargePositiveInteger class. Bitmap }.
show := [:v | v isFloat ifTrue: [ v isNaN ifTrue: ['NaN'] ifFalse: [ v printString, ' ', (v asIEEE64BitWord printStringBase: 16) ] ] ifFalse: [ v printString ] ].
out := WriteStream on: (String new: 100000).
n := 0.
classes do: [:c |
  (c selectors asSortedCollection: [:a :b | a <= b]) do: [:sel | | m rcvs argSets |
    m := c compiledMethodAt: sel.
    ((okMethod value: m) and: [ m numArgs <= 4 and: [ (#(#ensureDelete #become: #becomeForward: #pin #unpin) includes: sel) not ] ]) ifTrue: [
      rcvs := c isMeta ifTrue: [ { c soleInstance } ] ifFalse: [ vals select: [:r | r isKindOf: c ] ].
      argSets := m numArgs = 0 ifTrue: [ #(#()) ] ifFalse: [
        m numArgs = 1 ifTrue: [ vals collect: [:v | { v } ] ] ifFalse: [
          m numArgs = 2 ifTrue: [ small inject: OrderedCollection new into: [:acc :a | small do: [:b | acc add: { a. b } ]. acc ] ]
          ifFalse: [ (vals select: [:v | v isString or: [ v isKindOf: ByteArray ] ]) inject: OrderedCollection new into: [:acc :s |
               #(0 1 2 3) do: [:i | acc add: ((Array new: m numArgs) at: 1 put: s copy; yourself) ]. acc ] ] ] ].
      rcvs doWithIndex: [:r :ri |
        argSets doWithIndex: [:args :ai | | res rc |
          rc := r copy.
          res := [ (show value: (rc perform: sel withArguments: (args collect: [:x | x copy ]))), ' / ', (show value: rc) ]
             on: Error, ZeroDivide do: [:e | 'ERR ', e class name ].
          n := n + 1.
          out nextPutAll: c name; nextPutAll: '>>'; nextPutAll: sel; space; print: ri; space; print: ai; space;
            nextPutAll: (res size > 100 ifTrue: [ res first: 100 ] ifFalse: [ res ]); lf ] ] ] ] ].
f := (Smalltalk at: #PRIMDIFFOUT ifAbsent: [ '/tmp/primdiff-out.txt' ]) asFileReference.
f ensureDelete. f writeStreamDo: [:s | s nextPutAll: out contents ].
n
