"Diagnostic for BlockClosure test failures.
This script tests the basic operations needed by the failing tests."

| results |
results := WriteStream on: String new.
results nextPutAll: '=== BlockClosure Diagnostic ==='; cr.

"1. Basic block operations"
results nextPutAll: '--- Basic Block Operations ---'; cr.
[
    | block |
    block := [42].
    results nextPutAll: 'block value: ', block value printString; cr.
    results nextPutAll: 'block numArgs: ', block numArgs printString; cr.
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"2. Block with argument"
results nextPutAll: '--- Block with Argument ---'; cr.
[
    | block |
    block := [:x | x * 2].
    results nextPutAll: 'block numArgs: ', block numArgs printString; cr.
    results nextPutAll: 'block value: 5 = ', (block value: 5) printString; cr.
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"3. valueWithPossibleArgument:"
results nextPutAll: '--- valueWithPossibleArgument: ---'; cr.
[
    | block0 block1 |
    block0 := [99].
    block1 := [:x | x].
    results nextPutAll: 'block0 responds to valueWithPossibleArgument: ', (block0 respondsTo: #valueWithPossibleArgument:) printString; cr.
    (block0 respondsTo: #valueWithPossibleArgument:) ifTrue: [
        results nextPutAll: '[99] valueWithPossibleArgument: 5 = ', (block0 valueWithPossibleArgument: 5) printString; cr.
        results nextPutAll: '[:x | x] valueWithPossibleArgument: 5 = ', (block1 valueWithPossibleArgument: 5) printString; cr.
    ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"4. doWhileTrue: and doWhileFalse:"
results nextPutAll: '--- doWhileTrue: ---'; cr.
[
    | c |
    c := 0.
    results nextPutAll: 'responds to doWhileTrue: ', ([] respondsTo: #doWhileTrue:) printString; cr.
    ([] respondsTo: #doWhileTrue:) ifTrue: [
        [c := c + 1. c < 5] doWhileTrue: [].
        results nextPutAll: 'After doWhileTrue: c = ', c printString, ' (expected 5)'; cr.
    ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

results nextPutAll: '--- doWhileFalse: ---'; cr.
[
    | c |
    c := 0.
    ([] respondsTo: #doWhileFalse:) ifTrue: [
        [c := c + 1. c >= 5] doWhileFalse: [].
        results nextPutAll: 'After doWhileFalse: c = ', c printString, ' (expected 5)'; cr.
    ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"5. compiledBlock and hasMethodReturn"
results nextPutAll: '--- compiledBlock/hasMethodReturn ---'; cr.
[
    | block |
    block := [42].
    results nextPutAll: 'block class: ', block class name; cr.
    results nextPutAll: 'responds to compiledBlock: ', (block respondsTo: #compiledBlock) printString; cr.
    (block respondsTo: #compiledBlock) ifTrue: [
        | cb |
        cb := block compiledBlock.
        results nextPutAll: 'compiledBlock class: ', cb class name; cr.
        results nextPutAll: 'cb responds to hasMethodReturn: ', (cb respondsTo: #hasMethodReturn) printString; cr.
        (cb respondsTo: #hasMethodReturn) ifTrue: [
            results nextPutAll: 'compiledBlock hasMethodReturn: ', cb hasMethodReturn printString; cr.
        ].
    ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"6. hasNonLocalReturn"
results nextPutAll: '--- hasNonLocalReturn ---'; cr.
[
    | block blockNLR |
    block := [42].
    results nextPutAll: 'responds to hasNonLocalReturn: ', (block respondsTo: #hasNonLocalReturn) printString; cr.
    (block respondsTo: #hasNonLocalReturn) ifTrue: [
        results nextPutAll: '[42] hasNonLocalReturn: ', block hasNonLocalReturn printString; cr.
    ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"7. valueWithExit"
results nextPutAll: '--- valueWithExit ---'; cr.
[
    | result |
    results nextPutAll: 'responds to valueWithExit: ', ([:exit | ] respondsTo: #valueWithExit) printString; cr.
    ([:exit | ] respondsTo: #valueWithExit) ifFalse: [
        results nextPutAll: 'valueWithExit not available'; cr.
    ] ifTrue: [
        result := [:exit | 1 to: 10 do: [:i | i = 5 ifTrue: [exit value: i]]. 99] valueWithExit.
        results nextPutAll: 'valueWithExit result: ', result printString, ' (expected 5)'; cr.
    ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"8. Check BlockClosureTest class"
results nextPutAll: '--- BlockClosureTest availability ---'; cr.
[
    | testClass |
    testClass := Smalltalk at: #BlockClosureTest ifAbsent: [nil].
    testClass
        ifNil: [results nextPutAll: 'BlockClosureTest class NOT found'; cr]
        ifNotNil: [
            results nextPutAll: 'BlockClosureTest found'; cr.
            results nextPutAll: 'Number of test selectors: ', testClass allTestSelectors size printString; cr.
            results nextPutAll: 'Test selectors: ', (testClass allTestSelectors asArray first: (testClass allTestSelectors size min: 10)) printString; cr.
        ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

"9. Run testDoWhileTrue directly"
results nextPutAll: '--- Running testDoWhileTrue ---'; cr.
[
    | testClass instance |
    testClass := Smalltalk at: #BlockClosureTest ifAbsent: [nil].
    testClass ifNotNil: [
        instance := testClass new.
        [instance testDoWhileTrue.
         results nextPutAll: 'testDoWhileTrue PASSED'; cr.
        ] on: TestFailure do: [:ex |
            results nextPutAll: 'testDoWhileTrue FAILED: ', ex messageText; cr.
        ].
    ].
] on: Error do: [:e | results nextPutAll: 'ERROR: ', e class name, ' - ', e messageText; cr].

results nextPutAll: '=== Diagnostic Complete ==='; cr.

"/tmp/block_diag.txt" asFileReference writeStreamDo: [:s | s nextPutAll: results contents].

Transcript show: 'Diagnostic complete - see /tmp/block_diag.txt'; cr.
Smalltalk snapshot: false andQuit: true.
