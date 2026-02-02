"Test basic block operations - write results to /tmp/block_test_results.txt"
| stream |
stream := '/tmp/block_test_results.txt' asFileReference writeStream.
[
  "Test 1: Simple block value"
  | result |
  result := [1+1] value.
  stream nextPutAll: 'test1_simple_value: '; nextPutAll: (result = 2) printString; cr.

  "Test 2: Block value: with one arg"
  result := [:x | x * 2] value: 5.
  stream nextPutAll: 'test2_value_one_arg: '; nextPutAll: (result = 10) printString; cr.

  "Test 3: Block value:value: with two args"
  result := [:x :y | x + y] value: 3 value: 4.
  stream nextPutAll: 'test3_value_two_args: '; nextPutAll: (result = 7) printString; cr.

  "Test 4: Shared variable (temp vector)"
  | x |
  x := 0.
  [:v | x := v] value: 42.
  stream nextPutAll: 'test4_temp_vector: '; nextPutAll: (x = 42) printString; cr.

  "Test 5: Block with non-local return (in a method context)"
  result := OrderedCollection new.
  #(1 2 3) do: [:each | result add: each].
  stream nextPutAll: 'test5_do_block: '; nextPutAll: (result size = 3) printString; cr.

  "Test 6: Block with collect:"
  result := #(1 2 3) collect: [:each | each * 10].
  stream nextPutAll: 'test6_collect: '; nextPutAll: (result asArray = #(10 20 30)) printString; cr.

  "Test 7: should:raise: pattern (used by BlockClosureTest)"
  | caught |
  caught := false.
  [1/0] on: ZeroDivide do: [:e | caught := true].
  stream nextPutAll: 'test7_on_do: '; nextPutAll: caught printString; cr.

  "Test 8: Block numArgs"
  result := [:a :b | a + b] argumentCount.
  stream nextPutAll: 'test8_argumentCount: '; nextPutAll: (result = 2) printString; cr.

  "Test 9: whileTrue:"
  | count |
  count := 0.
  [count < 5] whileTrue: [count := count + 1].
  stream nextPutAll: 'test9_whileTrue: '; nextPutAll: (count = 5) printString; cr.

  "Test 10: ensure:"
  | ensureRan |
  ensureRan := false.
  [42] ensure: [ensureRan := true].
  stream nextPutAll: 'test10_ensure: '; nextPutAll: ensureRan printString; cr.

  stream nextPutAll: 'ALL_TESTS_COMPLETE'; cr.
] ensure: [stream close].
