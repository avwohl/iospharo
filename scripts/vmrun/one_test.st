"one_test.st -- run tests of one class and write one line per test.
 scripts/vmrun.sh test uses this file.
 Input:  ./runtest_args.txt in the working directory.  Line 1 is the class
         name.  Each next line is one selector.  No selector line means all
         tests of the class.  A line 'TIMEOUT=<seconds>' sets the limit of
         one test (default 120).
 Output: ./runtest_out.txt, one line per test:
           PASS|FAIL|ERROR|SKIP|TIMEOUT Class>>selector[ parameters] <ms>ms [text]
         and a last line DONE.
 The tests come from the suite of the class (buildSuite), so a parametrized
 test class runs each test with each parameter set, as SUnit does.  Each test
 runs with runCaseManaged (the SUnit execution environment, with its watchdog
 and process monitor).  The run is the same on our VM and on stock Cog."
| lines cls sels tests out limit emit |
"Same session state as the sweep runner (SUnitRunner class>>startUp:): the
 Morphic UI manager and the fake-GUI World cycle, when the image has them."
[ (UIManager default class == MorphicUIManager) ifFalse: [ MorphicUIManager new beDefault ] ] on: Error do: [ :e | ].
[ (Smalltalk globals at: #FakeGUI ifAbsent: [ nil ]) ifNotNil: [ :fg | fg perform: #startCycleLoop ] ] on: Error do: [ :e | ].
lines := ('runtest_args.txt' asFileReference readStreamDo: [ :s | s contents ]) lines
	collect: [ :l | l trimBoth ] thenSelect: [ :l | l notEmpty ].
limit := 120.
(lines select: [ :l | l beginsWith: 'TIMEOUT=' ]) do: [ :l | limit := (l copyAfter: $=) asInteger ].
lines := lines reject: [ :l | l beginsWith: 'TIMEOUT=' ].
cls := Smalltalk globals at: lines first asSymbol ifAbsent: [ nil ].
out := 'runtest_out.txt' asFileReference.
out ensureDelete.
emit := [ :text | out writeStreamDo: [ :o | o setToEnd; nextPutAll: text; nextPut: Character lf ] ].
cls
	ifNil: [ emit value: 'MISSING-CLASS ' , lines first ]
	ifNotNil: [
		sels := lines allButFirst collect: [ :l | l asSymbol ].
		tests := cls buildSuite tests asArray.
		sels isEmpty ifFalse: [ tests := tests select: [ :t | sels includes: t selector ] ].
		tests := tests asSortedCollection: [ :a :b | a selector <= b selector ].
		tests do: [ :test |
			| result text t0 ms label params |
			text := ''.
			params := [ test parametersToUse ] on: Error do: [ :e | #() ].
			label := cls name , '>>' , test selector.
			params isEmpty ifFalse: [ label := label , ' ' , params asArray printString ].
			t0 := Time millisecondClockValue.
			result := [ [ [ [ [ test runCaseManaged. #PASS ]
					valueWithin: limit seconds onTimeout: [ #TIMEOUT ] ]
				on: TestSkipped do: [ :ex | text := ex messageText ifNil: [ '' ]. #SKIP ] ]
				on: TestFailure do: [ :ex | text := ex messageText ifNil: [ '' ]. #FAIL ] ]
				on: Error do: [ :ex |
					text := ex class name , ': ' , (ex messageText ifNil: [ '' ]) , ' @ ' ,
						((ex signalerContext ifNotNil: [ :c | (c stack first: (8 min: c stack size)) collect: [ :f | f method printString ] ]) printString).
					#ERROR ] ]
				on: Halt do: [ :ex | text := 'Halt'. #ERROR ].
			ms := Time millisecondClockValue - t0.
			emit value: result , ' ' , label , ' ' , ms printString , 'ms' ,
				(text isEmpty ifTrue: [ '' ] ifFalse: [ ' [' , (text copyFrom: 1 to: (text size min: 900)) , ']' ]) ] ].
emit value: 'DONE'.
