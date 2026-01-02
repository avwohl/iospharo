/*
 * iosStubs.c
 *
 * Stub implementations for missing VM symbols on iOS.
 * These are needed because we're using interpreter-only mode (no JIT)
 * and some platform functions aren't available on iOS.
 */

#include <stddef.h>

/* JIT-related stubs - not needed for interpreter mode */
void voidCogCompiledCode(void) {
    /* No-op: JIT not available on iOS */
}

void* whereIsMaybeCodeThing(void* thing) {
    /* No-op: JIT not available on iOS */
    return NULL;
}

/* File dialog stubs - not available on iOS (use document picker instead) */
int vm_file_dialog_run_modal_open(void* dialog) {
    return 0;
}

int vm_file_dialog_run_modal_save(void* dialog) {
    return 0;
}

void* vm_file_dialog_create(const char* title, int type) {
    return NULL;
}

void vm_file_dialog_destroy(void* dialog) {}

const char* vm_file_dialog_get_filename(void* dialog) {
    return NULL;
}

void vm_file_dialog_set_filename(void* dialog, const char* filename) {}

void vm_file_dialog_add_filter(void* dialog, const char* description, const char* pattern) {}

int vm_file_dialog_is_nop(void) {
    return 1;
}

/* Error code utilities */
const char* vm_error_code_to_string(int code) {
    return "Unknown error";
}

/* Path conversion provided by sqUnixCharConv.c */

/* Main thread worker - iOS handles this differently */
void runMainThreadWorker(void) {}

/* String utilities */
void vm_string_append_into(char* dest, size_t destSize, const char* src) {
    if (dest && src && destSize > 0) {
        size_t destLen = 0;
        while (destLen < destSize - 1 && dest[destLen]) destLen++;
        while (destLen < destSize - 1 && *src) {
            dest[destLen++] = *src++;
        }
        dest[destLen] = '\0';
    }
}
