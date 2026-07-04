/*
 * TestLibrary.c - Pharo TFFI unit-test fixture library (clean-room).
 *
 * The Pharo image's TFUFFI* tests (TFUFFIStructuresTest,
 * TFUFFIMethodRegistryTest, TFUFFIFunctionCallTest, ...) do real callouts
 * into a `TestLibrary` shared library that upstream builds from the
 * pharo-vm repo. The fixture wasn't present on this machine, so all those
 * tests errored in setUp with SymbolNotFoundError. This is a from-scratch
 * implementation derived from the signatures and assertions in the image's
 * test code (extraction script: the ffi-sigs*.st probes, 2026-07-02).
 *
 * Struct layouts mirror the image-side TFFI definitions byte-for-byte
 * (sizes asserted by the tests: inner=16, nested=32, long=128 — `long` is
 * 4 bytes here, LLP64, matching TFFI's platform-long on Windows).
 *
 * Built by CMake into TestLibrary.dll next to test_load_image.exe
 * (FFIWindowsLibraryFinder probes `Smalltalk vm directory`).
 */

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define EXPORT __declspec(dllexport)
static void sleepSeconds(int s) { Sleep((DWORD)s * 1000); }
#else
#include <unistd.h>
#define EXPORT __attribute__((visibility("default")))
static void sleepSeconds(int s) { sleep((unsigned)s); }
#endif

typedef struct {
    int x;
    int y;
} TFPointTestStruct;

typedef struct {
    char a;
    void* b;
} TFInnerTestStruct;

typedef struct {
    TFInnerTestStruct inner;
    float x;
    double y;
} TFNestedTestStruct;

typedef struct {
    char a[100];
    float b;
    double c;
    long d;
    int e;
    short f;
} TFLongTestStruct;

/* ---- TFUFFIFunctionCallTest / TFUFFIMethodRegistryTest ---- */

EXPORT int shortCallout(void) {
    return 42;
}

EXPORT int multipleArgumentCallout(int a1, int a2, int a3, int a4, int a5,
                                   int a6, int a7, int a8, int a9, int a10) {
    return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10;
}

EXPORT int longCallout(int seconds) {
    sleepSeconds(seconds);
    return 42;
}

/* ---- TFUFFIStructuresTest ---- */

EXPORT int sizeOfNestedStruct(void) {
    return (int)sizeof(TFNestedTestStruct);
}

EXPORT int sizeOfLongStruct(void) {
    return (int)sizeof(TFLongTestStruct);
}

EXPORT int sizeOfPoint(void) {
    return (int)sizeof(TFPointTestStruct);
}

EXPORT int assertCorrectPoint(TFPointTestStruct p, int x, int y) {
    return (p.x == x && p.y == y) ? 1 : 0;
}

EXPORT TFPointTestStruct newPoint(int x, int y) {
    TFPointTestStruct p;
    p.x = x;
    p.y = y;
    return p;
}

EXPORT void* id_pointer(TFPointTestStruct* p) {
    return (void*)p;
}

EXPORT int passingNestedStruct(TFNestedTestStruct st, char a, double y) {
    return (st.inner.a == a && st.y == y) ? 1 : 0;
}

EXPORT int passingLongStruct(TFLongTestStruct st, float b, double c, long d) {
    return (st.b == b && st.c == c && st.d == d) ? 1 : 0;
}

EXPORT int passingLongStructByRef(TFLongTestStruct* st, float b, double c, long d) {
    if (!st) return 0;
    return (st->b == b && st->c == c && st->d == d) ? 1 : 0;
}

/* ---- TFUFFIBasicType{Marshalling,Size}Test / DerivedType* ---- */

#include <stdint.h>
#include <stdlib.h>

#define SUM_FN(name, type) EXPORT type name(type a, type b) { return (type)(a + b); }
SUM_FN(sum_char, char)
SUM_FN(sum_uchar, unsigned char)
SUM_FN(sum_short, short)
SUM_FN(sum_ushort, unsigned short)
SUM_FN(sum_int, int)
SUM_FN(sum_uint, unsigned int)
SUM_FN(sum_long, long)
SUM_FN(sum_ulong, unsigned long)
SUM_FN(sum_longlong, long long)
SUM_FN(sum_ulonglong, unsigned long long)
SUM_FN(sum_int8_t, int8_t)
SUM_FN(sum_uint8_t, uint8_t)
SUM_FN(sum_int16_t, int16_t)
SUM_FN(sum_uint16_t, uint16_t)
SUM_FN(sum_int32_t, int32_t)
SUM_FN(sum_uint32_t, uint32_t)
SUM_FN(sum_int64_t, int64_t)
SUM_FN(sum_uint64_t, uint64_t)
SUM_FN(sum_float, float)
SUM_FN(sum_double, double)
#undef SUM_FN

#define SIZEOF_FN(name, type) EXPORT size_t name(void) { return sizeof(type); }
SIZEOF_FN(sizeof_char, char)
SIZEOF_FN(sizeof_uchar, unsigned char)
SIZEOF_FN(sizeof_short, short)
SIZEOF_FN(sizeof_ushort, unsigned short)
SIZEOF_FN(sizeof_int, int)
SIZEOF_FN(sizeof_uint, unsigned int)
SIZEOF_FN(sizeof_long, long)
SIZEOF_FN(sizeof_ulong, unsigned long)
SIZEOF_FN(sizeof_longlong, long long)
SIZEOF_FN(sizeof_ulonglong, unsigned long long)
SIZEOF_FN(sizeof_int8_t, int8_t)
SIZEOF_FN(sizeof_uint8_t, uint8_t)
SIZEOF_FN(sizeof_int16_t, int16_t)
SIZEOF_FN(sizeof_uint16_t, uint16_t)
SIZEOF_FN(sizeof_int32_t, int32_t)
SIZEOF_FN(sizeof_uint32_t, uint32_t)
SIZEOF_FN(sizeof_int64_t, int64_t)
SIZEOF_FN(sizeof_uint64_t, uint64_t)
SIZEOF_FN(sizeof_float, float)
SIZEOF_FN(sizeof_double, double)
SIZEOF_FN(sizeof_pointer, void*)
SIZEOF_FN(sizeof_size_t, size_t)
/* enum flavors: a C enum is int-sized even with char-range values
   (TFUFFIBasicTypeSizeTest>>testSizeCharEnum asserts 4) */
SIZEOF_FN(sizeof_charenum, int)
SIZEOF_FN(sizeof_sintenum, int)
SIZEOF_FN(sizeof_uintenum, unsigned int)
#undef SIZEOF_FN

EXPORT int id_int(int x) { return x; }
EXPORT size_t id_size_t(size_t x) { return x; }
EXPORT void* unref_pointer(void* p) { return p; }

/* strdup for the marshalling tests; the image frees via its own machinery
   (or leaks — it is a unit-test fixture, matching upstream behavior). */
EXPORT char* dup_string(const char* s) {
    if (!s) return NULL;
    {
        size_t n = strlen(s) + 1;
        char* r = (char*)malloc(n);
        if (r) memcpy(r, s, n);
        return r;
    }
}

EXPORT void fillByteArray(char* bytes, int size) {
    int i;
    if (!bytes) return;
    for (i = 0; i < size; i++) bytes[i] = (char)(i + 1);
}

/* ===== TFFI callback fixtures (TFUFFICallbackTest + *InCallbacksTest +
   TFCallbacksTest) =====
   Semantics derived clean-room from the image tests (2026-07-03, corrected
   2026-07-04):
   - singleCallToCallback: return cb(value + 1) — increment the ARGUMENT and
     pass the callback's result through untouched.  This satisfies ALL
     image expectations simultaneously:
       * TFUFFICallbackTest (cb = [:a | a+1], value 3): cb(4) = 5.
       * TFCallbacksTest>>testSingleCalloutDuringCallback (cb answers a
         constant 42 via shortCallout, value 3): cb(4) = 42.
       * TFCallbacksTest reentrant tests (cb re-invokes with times+1 until
         7): the chain converges to exactly 7 only if the result is passed
         through unmodified.
     The earlier clean-room guess `cb(value) + 1` produced 5 for the first
     test by coincidence and broke the other two (3F same-thread).
   - callbackInALoop: with cb = a+1 the result must be 42 -> fold cb 42
     times starting from 0.
   - callbackFromAnotherThread: cb runs on a NATIVE thread (exercises the
     VM's cross-thread callback forwarding); stores cb(42) into a global
     that getValue() reads (test asserts 43 with cb = a+1). */

EXPORT int returnAnswer(void) { return 42; }
EXPORT int sumTwoNumbers(int a, int b) { return a + b; }
EXPORT float sumAFloatAndADouble(float a, double b) { return (float)(a + b); }

EXPORT int singleCallToCallback(int (*cb)(int), int value) {
    return cb(value + 1);
}

EXPORT int callbackInALoop(int (*cb)(int)) {
    int v = 0;
    int i;
    for (i = 0; i < 42; i++) v = cb(v);
    return v;
}

static volatile int tl_callbackThreadValue = 0;

EXPORT int getValue(void) { return tl_callbackThreadValue; }

#ifdef _WIN32
static DWORD WINAPI tl_callbackThreadMain(LPVOID arg) {
    int (*cb)(int) = (int (*)(int))arg;
    tl_callbackThreadValue = cb(42);
    return 0;
}
EXPORT void callbackFromAnotherThread(int (*cb)(int)) {
    HANDLE h = CreateThread(NULL, 0, tl_callbackThreadMain, (LPVOID)cb, 0, NULL);
    if (h) CloseHandle(h);
}
#else
#include <pthread.h>
static void* tl_callbackThreadMain(void* arg) {
    int (*cb)(int) = (int (*)(int))arg;
    tl_callbackThreadValue = cb(42);
    return NULL;
}
EXPORT void callbackFromAnotherThread(int (*cb)(int)) {
    pthread_t t;
    if (pthread_create(&t, NULL, tl_callbackThreadMain, (void*)cb) == 0)
        pthread_detach(t);
}
#endif

/* void simple_callback_f_T(void (*cb)(T), T value) { cb(value); } —
   the callback (image block) records the received value; tests assert the
   round-trip. One symbol per marshalled type. */
#define SIMPLE_CB(NAME, T) \
    EXPORT void simple_callback_f_##NAME(void (*cb)(T), T value) { cb(value); }
SIMPLE_CB(char, char)
SIMPLE_CB(uchar, unsigned char)
SIMPLE_CB(int8_t, int8_t)
SIMPLE_CB(uint8_t, uint8_t)
SIMPLE_CB(short, short)
SIMPLE_CB(ushort, unsigned short)
SIMPLE_CB(int16_t, int16_t)
SIMPLE_CB(uint16_t, uint16_t)
SIMPLE_CB(int, int)
SIMPLE_CB(uint, unsigned int)
SIMPLE_CB(int32_t, int32_t)
SIMPLE_CB(uint32_t, uint32_t)
SIMPLE_CB(long, long)
SIMPLE_CB(ulong, unsigned long)
SIMPLE_CB(longlong, long long)
SIMPLE_CB(ulonglong, unsigned long long)
SIMPLE_CB(int64_t, int64_t)
SIMPLE_CB(uint64_t, uint64_t)
SIMPLE_CB(float, float)
SIMPLE_CB(double, double)
SIMPLE_CB(size_t, size_t)
SIMPLE_CB(pointer, void*)
#undef SIMPLE_CB
