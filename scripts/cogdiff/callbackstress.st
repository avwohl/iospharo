"callbackstress.st -- same-thread FFI callbacks: a qsort loop and nested callbacks,
 with a ticking background process (timer signals all through the run).
 Run as the eval argument on our VM and on Cog; both answer
 STRESSRESULT {#qsortLoop->#(0 true). #nested->#(0 true true)} ticking=true
 2026-11-13: main 6efa5c9c hangs (arm64 and x86_64, 3 of 3); the callback
 return as in Cog answers in 20 s (our VM) and 3 s (Cog)."
| L r run q cb inner innerCb ticks stop |
(Object << #CbStressLib package: 'CbStress') install.
L := Smalltalk at: #CbStressLib.
L class compile: 'ffiLibraryName ^ LibC'.
L class compile: 'qsort: a n: n size: sz cb: c ^ self ffiCall: #(void qsort(void* a, size_t n, size_t sz, FFICallback c))'.
r := OrderedCollection new.
run := [:name :blk | r add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name, ' ', e messageText asString ]) ].
"a ticking process at timer priority gives the VM timer wakeups all through the run"
ticks := 0. stop := false.
[ [ stop ] whileFalse: [ (Delay forMilliseconds: 1) wait. ticks := ticks + 1 ] ] forkAt: Processor timingPriority - 1.
"1. qsort loop: sort 64 pseudo-random ints, 300 times, with a Smalltalk comparator"
run value: #qsortLoop value: [ | ext n bad calls rnd |
  n := 64. ext := ExternalAddress allocate: n * 4. bad := 0. calls := 0. rnd := 12345.
  cb := FFICallback signature: #(int (void* a, void* b)) block: [:a :b | calls := calls + 1. (a signedLongAt: 1) - (b signedLongAt: 1) ].
  [ 1 to: 300 do: [:round |
      1 to: n do: [:i | rnd := (rnd * 1103515245 + 12345) \\ 2147483648. ext signedLongAt: (i - 1) * 4 + 1 put: rnd \\ 100000 ].
      L qsort: ext n: n size: 4 cb: cb.
      2 to: n do: [:i | (ext signedLongAt: (i - 2) * 4 + 1) <= (ext signedLongAt: (i - 1) * 4 + 1) ifFalse: [ bad := bad + 1 ] ] ].
    { bad. calls > 0 } ] ensure: [ ext free ] ].
"2. nested: the comparator of the outer qsort sorts a 4-element array with an inner qsort (callout inside a callback, which calls back)"
run value: #nested value: [ | ext iext n bad innerCalls outerCalls |
  n := 8. ext := ExternalAddress allocate: n * 4. iext := ExternalAddress allocate: 16. bad := 0. innerCalls := 0. outerCalls := 0.
  innerCb := FFICallback signature: #(int (void* a, void* b)) block: [:a :b | innerCalls := innerCalls + 1. (b signedLongAt: 1) - (a signedLongAt: 1) ].
  cb := FFICallback signature: #(int (void* a, void* b)) block: [:a :b |
     outerCalls := outerCalls + 1.
     #(4 1 3 2) doWithIndex: [:x :i | iext signedLongAt: (i - 1) * 4 + 1 put: x ].
     L qsort: iext n: 4 size: 4 cb: innerCb.
     (iext signedLongAt: 1) = 4 ifFalse: [ bad := bad + 1 ].
     (a signedLongAt: 1) - (b signedLongAt: 1) ].
  [ 1 to: 100 do: [:round |
      #(8 3 7 1 6 2 5 4) doWithIndex: [:x :i | ext signedLongAt: (i - 1) * 4 + 1 put: x ].
      L qsort: ext n: n size: 4 cb: cb.
      (1 to: n) do: [:i | (ext signedLongAt: (i - 1) * 4 + 1) = i ifFalse: [ bad := bad + 1 ] ] ].
    { bad. innerCalls > 0. outerCalls > 0 } ] ensure: [ ext free. iext free ] ].
stop := true.
'STRESSRESULT ', r asArray printString, ' ticking=', (ticks > 0) printString
