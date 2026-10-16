"edgediff.st -- scenarios run on our VM and on Cog; diff the two answers.
Run: test_load_image P.image eval "$(cat edgediff.st)"; for Cog, arch -x86_64 Pharo --headless C.image eval ...
See docs/vm-compat-bugs.md, 2026-10-17."

| exprs out |
exprs := #(
 '(Array new: -1) size' '(String new: -5) size' '(ByteArray new: (2 raisedTo: 62)) size' '(Array new: 3.5) size' '(Array new: nil) size'
 '(Character value: -1) asInteger' '(Character value: 16r10FFFF) asInteger' '(Character value: 16r110000) asInteger' '(Character value: (2 raisedTo: 40)) asInteger' '$a asInteger' '(Character value: 3.0) asInteger'
 'Object new instVarAt: 0' 'Object new instVarAt: 1' '(3@4) instVarAt: 2' '(3@4) instVarAt: 3' '3 instVarAt: 1' '#(1 2) instVarAt: 1' '((3@4) instVarAt: 1 put: 9; yourself)'
 '#(1 2) at: 0' '#(1 2) at: 3' '#(1 2) at: 1.0' '#(1 2) at: (2 raisedTo: 70)' '#(1 2) at: nil' '''abc'' at: 4' '#[1 2 3] at: 2' '(1 to: 5) at: 6'
 '''abc'' copy at: 1 put: (Character value: 300); yourself' '(''abc'' copy at: 1 put: 65; yourself)' '(#[1 2] copy at: 1 put: 256; yourself)' '(#[1 2] copy at: 1 put: -1; yourself)' '(#[1 2] copy at: 1 put: $a; yourself)'
 '#(1 2 3) at: 1 put: 9' '#sym at: 1 put: $x' '''lit'' at: 1 put: $x'
 '3 become: 4' 'nil shallowCopy' '1.5 shallowCopy == 1.5' '(2 raisedTo: 100) shallowCopy = (2 raisedTo: 100)' '$a shallowCopy'
 '1/0' '1.0/0' '1.0/0.0' '0.0/0.0' '(2 raisedTo: 100) // 0' '5 \\ 0' '5 rem: 0' '1.0e308 * 10' 'Float nan = Float nan' 'Float nan == Float nan'
 '-7 \\ 2' '-7 rem: 2' '-7 // 2' '-7 quo: 2' '7 \\ -2' '(2 raisedTo: 64) sqrt' '(2 raisedTo: 64) sqrtFloor' '100 factorial printString size' '1000000000 * 1000000000 * 1000000000'
 'SmallInteger maxVal + 1' '(SmallInteger maxVal + 1) class' 'SmallInteger minVal - 1' '(SmallInteger minVal // -1) class' 'SmallInteger minVal negated class' 'SmallInteger minVal abs class'
 '1 bitShift: 64' '-1 bitShift: -100' '(2 raisedTo: 100) bitShift: -98' '(2 raisedTo: 100) negated bitShift: -98' '5 bitAnd: (2 raisedTo: 100) - 1' '-5 bitAnd: (2 raisedTo: 100)' '(2 raisedTo: 100) negated bitOr: 7' '-1 bitXor: (2 raisedTo: 65)'
 '3.7 truncated' '-3.7 truncated' '3.7 rounded' '-3.5 rounded' '1.0e20 truncated' '1.0e20 truncated class' 'Float infinity truncated' 'Float nan truncated' '2.5 fractionPart' '-2.5 floor' '2 ln' '0 ln' '-1 ln' '0.0 ln' '-1.0 sqrt' '1.0e-320 exponent' '1.0e-320 timesTwoPower: 1000' '1.0 timesTwoPower: 2000' '3.0 timesTwoPower: -1080'
 '1.0e15 printString' '0.1 printString' '123.456e-7 printString' '(1/3) asFloat printString' '2 sqrt printString' '1.0e-310 printString' '100 factorial asFloat'
 '(Array new: 3 withAll: 7)' '(1 to: 0) asArray' '''abc'' asSymbol == #abc' '#(1 $a ''b'' #c 2.5) hash' '''hello'' hash' '#hello hash' '(1 to: 10) asArray hash' '3.5 hash' '(2 raisedTo: 100) hash' '$a hash' 'nil hash' 'true hash'
 '''abc'' < ''abd''' '''abc'' = #abc' '#abc = ''abc''' '''abc'' asWideString = ''abc''' '''ABC'' sameAs: ''abc''' '''abc'' caseSensitiveLessOrEqual: ''ABC''' '''b'' max: ''a'''
 '(Smalltalk vm parameterAt: 40)' 'Smalltalk vm wordSize' 'Smalltalk vm isRunningCogit' 'Smalltalk vm imageFormatVersion' 'SmallInteger maxVal' 'SmallFloat64 maxVal' '(1 to: 3) collect: [:i | i perform: #+ withArguments: #(1 2)]' '3 perform: #between:and: with: 1' '3 perform: #foo'
 'Object new perform: #yourself withArguments: 3' '3 perform: 42' '3 perform: #+ withArguments: #(4)' '#(3 4) perform: #at: with: 1' '(Message selector: #+ arguments: #(1)) sendTo: 2'
 '[:a | a] value: 1 value: 2' '[:a | a] valueWithArguments: #(1 2)' '[] value: 3' '[:a :b | a + b] valueWithArguments: #(1 2)' '[^ 3] value' '[:x | x] numArgs' '(1 to: 5) inject: 0 into: [:a :b | a + b]'
 'thisContext isDead' 'thisContext sender isNil' '(thisContext tempAt: 1) class' 'thisContext method class' 'thisContext pc class'
 'Processor activePriority' 'Processor activeProcess priority' 'Semaphore new signal; excessSignals' '(Semaphore new) wait; yourself'
).
out := WriteStream on: String new.
exprs do: [:e | | r |
  r := [ (Smalltalk compiler evaluate: e) printString ] on: Error, ZeroDivide, Warning do: [:ex | 'ERR ', ex class name, ' ', (ex messageText ifNil: [''] ifNotNil: [:t | t asString first: (t size min: 40)]) ].
  out nextPutAll: e; nextPutAll: ' => '; nextPutAll: (r size > 120 ifTrue: [r first: 120] ifFalse: [r]); lf ].
(Smalltalk at: #EDGEOUT ifAbsent: ['/tmp/edgediff-out.txt']) asFileReference ensureDelete; writeStreamDo: [:s | s nextPutAll: out contents].
exprs size
