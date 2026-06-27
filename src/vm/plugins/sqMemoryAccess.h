/* Minimal sqMemoryAccess.h for plugin compilation (64-bit ARM64) */
#ifndef SQ_MEMORY_ACCESS_H
#define SQ_MEMORY_ACCESS_H

#include <stdint.h>
#include <string.h>

/* 64-bit image on 64-bit host */
#define SQ_IMAGE64 1
#define SQ_HOST64 1

/* sqInt MUST be pointer-sized (it holds oops/pointers).  `long` is 64-bit on
 * LP64 (Linux/macOS/ARM64) but only 32-bit on Windows (LLP64), which truncated
 * every oop passed through the plugin/InterpreterProxy interface (e.g.
 * proxy_isBytes(sqInt) -> garbage ObjectHeader* -> crash).  intptr_t/int64_t
 * are 64-bit on all 64-bit hosts and identical to `long` on LP64, so this is
 * byte-for-byte unchanged on Linux/macOS and fixes Windows. */
typedef intptr_t sqInt;
typedef uintptr_t usqInt;

#define sqLong int64_t
#define usqLong uint64_t

typedef intptr_t sqIntptr_t;
typedef uintptr_t usqIntptr_t;

/* Memory access macros - direct access since same endianness */
#define byteAtPointer(ptr)       ((unsigned char)(*(ptr)))
#define byteAtPointerput(ptr,v)  (*(ptr) = (unsigned char)(v))

#define longAt(oop)              (*((sqInt *)(oop)))
#define longAtput(oop,v)         (*((sqInt *)(oop)) = (v))

#define intAt(oop)               (*((int *)(oop)))
#define intAtput(oop,v)          (*((int *)(oop)) = (v))

#define long32At(oop)            (*((int *)(oop)))
#define long32Atput(oop,v)       (*((int *)(oop)) = (v))

#define long64At(oop)            (*((sqLong *)(oop)))
#define long64Atput(oop,v)       (*((sqLong *)(oop)) = (v))

/* Pointer/OOP conversions - identity on 64-bit */
#define oopForPointer(ptr)       ((sqInt)(ptr))
#define pointerForOop(oop)       ((char *)(oop))

/* Float access */
static inline void storeFloatAtPointerfrom(char *ptr, double val) {
    memcpy(ptr, &val, sizeof(double));
}
static inline void fetchFloatAtPointerinto(char *ptr, double *val) {
    memcpy(val, ptr, sizeof(double));
}
static inline void storeSingleFloatAtPointerfrom(char *ptr, float val) {
    memcpy(ptr, &val, sizeof(float));
}
static inline void fetchSingleFloatAtPointerinto(char *ptr, float *val) {
    memcpy(val, ptr, sizeof(float));
}

#define flag(foo) 0

#endif /* SQ_MEMORY_ACCESS_H */
