"ffidiff.st -- libc callouts run on our VM and on Cog; diff the two answers.
Run: test_load_image P.image eval "$(cat ffidiff.st)"; for Cog, arch -x86_64 Pharo --headless C.image eval ...
#snprintf and #snprintfMany declare a variadic function without fixedArgumentCount:.
They work on x86-64 only (variadic arguments go in registers there); on arm64
Darwin variadic arguments go on the stack, so the answers are garbage on any VM.
The #v... forms declare fixedArgumentCount: 3 and must match everywhere."

| r run buf |
(Object << #FFIDiffLib package: 'FFIDiff') install.
(Smalltalk at: #FFIDiffLib) class compile: 'vsnprintf: buf size: n fmt: f int: i str: s dbl: d ^ self ffiCall: #(int snprintf(ByteArray buf, size_t n, String f, int i, String s, double d)) fixedArgumentCount: 3'.
(Smalltalk at: #FFIDiffLib) class compile: 'vsnprintf2: buf size: n fmt: f a: a b: b c: c d: d e: e f: g g: h h: k i: l ^ self ffiCall: #(int snprintf(ByteArray buf, size_t n, String f, int a, int b, int c, int d, int e, int g, int h, int k, int l)) fixedArgumentCount: 3'.
(Smalltalk at: #FFIDiffLib) class compile: 'ffiLibraryName ^ LibC'.
(Smalltalk at: #FFIDiffLib) class compile: 'strlen: s ^ self ffiCall: #(size_t strlen(String s))'.
(Smalltalk at: #FFIDiffLib) class compile: 'abs: i ^ self ffiCall: #(int abs(int i))'.
(Smalltalk at: #FFIDiffLib) class compile: 'labs: i ^ self ffiCall: #(long labs(long i))'.
(Smalltalk at: #FFIDiffLib) class compile: 'atof: s ^ self ffiCall: #(double atof(String s))'.
(Smalltalk at: #FFIDiffLib) class compile: 'toupper: c ^ self ffiCall: #(int toupper(int c))'.
(Smalltalk at: #FFIDiffLib) class compile: 'strtol: s base: b ^ self ffiCall: #(long strtol(String s, nil, int b))'.
(Smalltalk at: #FFIDiffLib) class compile: 'memcmp: a with: b size: n ^ self ffiCall: #(int memcmp(ByteArray a, ByteArray b, size_t n))'.
(Smalltalk at: #FFIDiffLib) class compile: 'ldexp: x exp: e ^ self ffiCall: #(double ldexp(double x, int e))'.
(Smalltalk at: #FFIDiffLib) class compile: 'fabsf: x ^ self ffiCall: #(float fabsf(float x))'.
(Smalltalk at: #FFIDiffLib) class compile: 'snprintf: buf size: n fmt: f int: i str: s dbl: d ^ self ffiCall: #(int snprintf(ByteArray buf, size_t n, String f, int i, String s, double d))'.
(Smalltalk at: #FFIDiffLib) class compile: 'snprintf2: buf size: n fmt: f a: a b: b c: c d: d e: e f: g g: h h: k i: l ^ self ffiCall: #(int snprintf(ByteArray buf, size_t n, String f, int a, int b, int c, int d, int e, int g, int h, int k, int l))'.
(Smalltalk at: #FFIDiffLib) class compile: 'qsortBytes: arr ^ self ffiCall: #(void qsort(ByteArray arr, size_t 4, size_t 1, nil))'.
r := OrderedCollection new.
run := [:name :blk | r add: name -> ([ blk value ] on: Error do: [:e | 'ERR ', e class name ]) ].
run value: #strlen value: [ (Smalltalk at: #FFIDiffLib) strlen: 'hello world' ].
run value: #abs value: [ { (Smalltalk at: #FFIDiffLib) abs: -42. (Smalltalk at: #FFIDiffLib) abs: 2147483647 } ].
run value: #labs value: [ (Smalltalk at: #FFIDiffLib) labs: -12345678901 ].
run value: #atof value: [ (Smalltalk at: #FFIDiffLib) atof: '3.25e2' ].
run value: #toupper value: [ (Smalltalk at: #FFIDiffLib) toupper: $a asInteger ].
run value: #strtol value: [ { (Smalltalk at: #FFIDiffLib) strtol: 'ff' base: 16. (Smalltalk at: #FFIDiffLib) strtol: '-777' base: 8 } ].
run value: #memcmp value: [ { ((Smalltalk at: #FFIDiffLib) memcmp: #[1 2 3] with: #[1 2 4] size: 3) sign. (Smalltalk at: #FFIDiffLib) memcmp: #[5] with: #[5] size: 1 } ].
run value: #ldexp value: [ (Smalltalk at: #FFIDiffLib) ldexp: 1.5 exp: 10 ].
run value: #fabsf value: [ (Smalltalk at: #FFIDiffLib) fabsf: -2.5 ].
run value: #snprintf value: [ buf := ByteArray new: 64. { (Smalltalk at: #FFIDiffLib) snprintf: buf size: 64 fmt: '%d|%s|%.2f' int: 42 str: 'hi' dbl: 3.14159. (buf copyUpTo: 0) asString } ].
run value: #snprintfMany value: [ buf := ByteArray new: 64. { (Smalltalk at: #FFIDiffLib) snprintf2: buf size: 64 fmt: '%d %d %d %d %d %d %d %d %d' a: 1 b: 2 c: 3 d: 4 e: 5 f: 6 g: 7 h: 8 i: 9. (buf copyUpTo: 0) asString } ].
run value: #vsnprintf value: [ buf := ByteArray new: 64. { (Smalltalk at: #FFIDiffLib) vsnprintf: buf size: 64 fmt: '%d|%s|%.2f' int: 42 str: 'hi' dbl: 3.14159. (buf copyUpTo: 0) asString } ].
run value: #vsnprintfMany value: [ buf := ByteArray new: 64. { (Smalltalk at: #FFIDiffLib) vsnprintf2: buf size: 64 fmt: '%d %d %d %d %d %d %d %d %d' a: 1 b: 2 c: 3 d: 4 e: 5 f: 6 g: 7 h: 8 i: 9. (buf copyUpTo: 0) asString } ].
r asArray
