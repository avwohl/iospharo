/* Minimal sqMemoryAccess.h for plugin compilation (64-bit ARM64) */
#ifndef SQ_MEMORY_ACCESS_H
#define SQ_MEMORY_ACCESS_H

#include <stdint.h>
#include <string.h>

/* 64-bit image on 64-bit host */
#define SQ_IMAGE64 1
#define SQ_HOST64 1

typedef long sqInt;
typedef unsigned long usqInt;

#define sqLong long
#define usqLong unsigned long

typedef long sqIntptr_t;
typedef unsigned long usqIntptr_t;

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
