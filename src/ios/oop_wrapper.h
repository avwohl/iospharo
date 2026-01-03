/*
 * oop_wrapper.h - C interface for C++ Oop wrapper
 *
 * These functions can be called from C code to use space-aware
 * memory operations implemented in C++.
 */

#ifndef OOP_WRAPPER_H
#define OOP_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Space encoding during image load (swizzle) */
uint64_t oop_encode_with_space(uint64_t rawAddress, int space);

/* Space checking - returns 1 for true, 0 for false */
int oop_is_young(uint64_t bits);
int oop_is_old(uint64_t bits);
int oop_is_permanent(uint64_t bits);
int oop_is_immediate(uint64_t bits);
int oop_is_non_immediate(uint64_t bits);

/* Integer operations */
int oop_is_small_integer(uint64_t bits);
int64_t oop_integer_value_of(uint64_t bits);
uint64_t oop_integer_object_of(int64_t value);

/* Get raw pointer from oop (clears space bits) */
void* oop_to_pointer(uint64_t bits);

/* Get space as int (0=new, 1=old, 2=perm) */
int oop_get_space(uint64_t bits);

/* Space constants */
#define OOP_SPACE_NEW  0
#define OOP_SPACE_OLD  1
#define OOP_SPACE_PERM 2

#ifdef __cplusplus
}
#endif

#endif /* OOP_WRAPPER_H */
