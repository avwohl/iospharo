"ffidiff2.st -- FFI struct returns, struct pointers, out-pointers and callbacks,
run on our VM and on Cog; diff the two answers.  All equal on 2026-10-17
(our arm64 and x86-64 builds against Cog x86-64)."

| r run L D LD TM cb arr ep endp t |
(FFIExternalStructure << #FFIDiffDiv slots: {}; package: 'FFIDiff') install.
(FFIExternalStructure << #FFIDiffLDiv slots: {}; package: 'FFIDiff') install.
(FFIExternalStructure << #FFIDiffTm slots: {}; package: 'FFIDiff') install.
(Object << #FFIDiffLib2 package: 'FFIDiff') install.
D := Smalltalk at: #FFIDiffDiv. LD := Smalltalk at: #FFIDiffLDiv. TM := Smalltalk at: #FFIDiffTm. L := Smalltalk at: #FFIDiffLib2.
D class compile: 'fieldsDesc ^ #(int quot; int rem)'.
LD class compile: 'fieldsDesc ^ #(long quot; long rem)'.
TM class compile: 'fieldsDesc ^ #(int sec; int min; int hour; int mday; int mon; int year; int wday; int yday; int isdst; long gmtoff; char* zone)'.
D rebuildFieldAccessors. LD rebuildFieldAccessors. TM rebuildFieldAccessors.
L class compile: 'ffiLibraryName ^ LibC'.
L class compile: 'div: a by: b ^ self ffiCall: #(FFIDiffDiv div(int a, int b))'.
L class compile: 'ldiv: a by: b ^ self ffiCall: #(FFIDiffLDiv ldiv(long a, long b))'.
L class compile: 'gmtime: t into: tm ^ self ffiCall: #(FFIDiffTm* gmtime_r(ByteArray t, FFIDiffTm* tm))'.
L class compile: 'strtod: s end: e ^ self ffiCall: #(double strtod(String s, void** e))'.
L class compile: 'qsort: a n: n size: sz cb: c ^ self ffiCall: #(void qsort(void* a, size_t n, size_t sz, FFICallback c))'.
r := OrderedCollection new.
run := [:name :blk | r add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name, ' ', e messageText asString ]) ].
run value: #div value: [ | d | d := L div: 17 by: 5. { d quot. d rem } ].
run value: #divNeg value: [ | d | d := L div: -17 by: 5. { d quot. d rem } ].
run value: #ldiv value: [ | d | d := L ldiv: 123456789012345 by: 1000. { d quot. d rem } ].
run value: #gmtime value: [ | tm res | t := ByteArray new: 8. t unsignedLongLongAt: 1 put: 1000000000. tm := TM externalNew. res := L gmtime: t into: tm. { tm year. tm mon. tm mday. tm hour. tm min. tm sec. tm wday. tm yday. res getHandle = tm getHandle } ].
run value: #strtod value: [ | e v | e := ExternalAddress newPointer. v := L strtod: '2.5e3xyz' end: e. { v } ].
run value: #qsort value: [ | ext n calls |
  n := 6. ext := ExternalAddress allocate: n * 4. calls := 0.
  #(5 3 9 1 7 2) doWithIndex: [:x :i | ext signedLongAt: (i - 1) * 4 + 1 put: x ].
  cb := FFICallback signature: #(int (void* a, void* b)) block: [:a :b | calls := calls + 1. (a signedLongAt: 1) - (b signedLongAt: 1) ].
  L qsort: ext n: n size: 4 cb: cb.
  [ { (1 to: n) collect: [:i | ext signedLongAt: (i - 1) * 4 + 1 ]. calls > 0 } ] ensure: [ ext free ] ].
run value: #qsortNonLocal value: [ | ext n res |
  n := 4. ext := ExternalAddress allocate: n * 4.
  #(4 3 2 1) doWithIndex: [:x :i | ext signedLongAt: (i - 1) * 4 + 1 put: x ].
  cb := FFICallback signature: #(int (void* a, void* b)) block: [:a :b | (a signedLongAt: 1) - (b signedLongAt: 1) ].
  res := (1 to: 50) inject: 0 into: [:acc :k | L qsort: ext n: n size: 4 cb: cb. acc + (ext signedLongAt: 1) ].
  [ res ] ensure: [ ext free ] ].
run value: #cbDouble value: [ | c | c := FFICallback signature: #(double (double x, int k)) block: [:x :k | x * k ]. (c getHandle) isNull not ].
r asArray
