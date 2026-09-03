"Probe for open defect #22 (RSLinesTest BlockCannotReturn).

 The inline non-local return resolves a block's home by matching the METHOD
 OOP of the block's static outerCode literal against the frames on the stack
 (Interpreter.cpp:8470).  RSAbstractLine and the trait RSTMarkeable each hold a
 definition of markersIncludesPoint: and markerShapesInPositionDo:, so the
 question is whether the class's CompiledBlock names the class's method or the
 trait's.  If it names the trait's, the match cannot succeed against the frame
 actually on the stack and the VM sends cannotReturn: on a live home.

 Run: build-rel/test_load_image <image> eval ""'<this file>' asFileReference fileIn""
 or paste the body into an eval.  Writes /tmp/trait_block_home.txt."
| out report |
report := [ :cls :sel |
    | m blocks |
    m := (Smalltalk at: cls ifAbsent: [ nil ])
            ifNil: [ nil ]
            ifNotNil: [ :c | c compiledMethodAt: sel ifAbsent: [ nil ] ].
    m
        ifNil: [ out nextPutAll: cls; nextPutAll: ' >> '; nextPutAll: sel;
                 nextPutAll: '  ABSENT'; cr ]
        ifNotNil: [
            out nextPutAll: cls asString; nextPutAll: ' >> '; nextPutAll: sel asString; cr.
            out nextPutAll: '   method oop     = '; nextPutAll: m identityHash printString; cr.
            out nextPutAll: '   methodClass    = '; nextPutAll: m methodClass printString; cr.
            blocks := m literals select: [ :l | l class name = 'CompiledBlock' ].
            out nextPutAll: '   inner blocks   = '; nextPutAll: blocks size printString; cr.
            blocks do: [ :b |
                out nextPutAll: '     block outerCode  = ';
                    nextPutAll: b outerCode printString; cr.
                out nextPutAll: '     outerCode == m   = ';
                    nextPutAll: (b outerCode == m) printString; cr ] ] ].
out := WriteStream on: String new.
report value: #RSAbstractLine value: #markersIncludesPoint:.
report value: #RSTMarkeable  value: #markersIncludesPoint:.
report value: #RSAbstractLine value: #markerShapesInPositionDo:.
report value: #RSTMarkeable  value: #markerShapesInPositionDo:.
'/tmp/trait_block_home.txt' asFileReference writeStreamDo: [ :s |
    s nextPutAll: out contents ].
Transcript show: out contents; cr.
