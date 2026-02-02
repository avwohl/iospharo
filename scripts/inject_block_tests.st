"Inject block tests that run at image startup, writing results to /tmp/block_test_results.txt"

SessionManager default register: (ClassSessionHandler forClassNamed: #BlockTestRunner) atPriority: 90.

Object subclass: #BlockTestRunner
  instanceVariableNames: ''
  classVariableNames: ''
  package: 'Tests-Custom'.

BlockTestRunner class compile: 'startUp: resuming
  resuming ifFalse: [^ self].
  self runTests'.

BlockTestRunner class compile: 'shutDown: quitting'.

BlockTestRunner class compile: 'runTests
  | stream |
  stream := StandardFileStream new open: ''/tmp/block_test_results.txt'' forWrite: true.
  stream ifNil: [^ self].
  [self runAllTests: stream] ensure: [stream close].
  Smalltalk exitSuccess'.

BlockTestRunner class compile: 'runAllTests: s
  | result |
  "Test 1: Simple block value"
  result := [1+1] value.
  s nextPutAll: ''test1_simple_value: ''; nextPutAll: (result = 2) printString; cr.

  "Test 2: Block value: with one arg"
  result := [:x | x * 2] value: 5.
  s nextPutAll: ''test2_value_one_arg: ''; nextPutAll: (result = 10) printString; cr.

  "Test 3: Block value:value:"
  result := [:x :y | x + y] value: 3 value: 4.
  s nextPutAll: ''test3_value_two_args: ''; nextPutAll: (result = 7) printString; cr.

  "Test 4: Shared variable (temp vector)"
  | x |
  x := 0.
  [:v | x := v] value: 42.
  s nextPutAll: ''test4_temp_vector: ''; nextPutAll: (x = 42) printString; cr.

  "Test 5: do: block"
  result := OrderedCollection new.
  #(1 2 3) do: [:each | result add: each].
  s nextPutAll: ''test5_do_block: ''; nextPutAll: (result size = 3) printString; cr.

  "Test 6: collect:"
  result := #(1 2 3) collect: [:each | each * 10].
  s nextPutAll: ''test6_collect: ''; nextPutAll: (result asArray = #(10 20 30)) printString; cr.

  "Test 7: on:do: exception handler"
  | caught |
  caught := false.
  [1/0] on: ZeroDivide do: [:e | caught := true].
  s nextPutAll: ''test7_on_do: ''; nextPutAll: caught printString; cr.

  "Test 8: argumentCount"
  result := [:a :b | a + b] argumentCount.
  s nextPutAll: ''test8_argumentCount: ''; nextPutAll: (result = 2) printString; cr.

  "Test 9: whileTrue:"
  | count |
  count := 0.
  [count < 5] whileTrue: [count := count + 1].
  s nextPutAll: ''test9_whileTrue: ''; nextPutAll: (count = 5) printString; cr.

  "Test 10: ensure:"
  | ensureRan |
  ensureRan := false.
  [42] ensure: [ensureRan := true].
  s nextPutAll: ''test10_ensure: ''; nextPutAll: ensureRan printString; cr.

  "Test 11: Non-local return from block"
  result := self testNonLocalReturn.
  s nextPutAll: ''test11_non_local_return: ''; nextPutAll: (result = 42) printString; cr.

  s nextPutAll: ''ALL_TESTS_COMPLETE''; cr'.

BlockTestRunner class compile: 'testNonLocalReturn
  #(1 42 3) do: [:each |
    each = 42 ifTrue: [^ each]].
  ^ 0'.
