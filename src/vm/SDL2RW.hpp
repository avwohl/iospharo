/*
 * SDL2RW.hpp - SDL_RWops data streams over files and memory, and the
 * endian read/write helpers of SDL_rwops.h.
 *
 * The SDL_RWops structure has the SDL 2.0.x layout, because the image can
 * allocate one with SDL_AllocRW and fill the function pointers itself (a
 * Smalltalk callback then serves as read, write, seek, size or close).  The
 * wrapper functions SDL_RWread and the others only call the pointer, as the
 * real ones do.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Base.hpp"

namespace pharo {
namespace sdl2 {
namespace rw {

using sys::setError;
using sys::invalidParam;

enum RWType : uint32_t { kTypeUnknown = 0, kTypeStdFile = 2, kTypeMemory = 4, kTypeMemoryReadOnly = 5 };
enum Whence : int { kSeekSet = 0, kSeekCur = 1, kSeekEnd = 2 };
enum ErrorCode : int { kErrNoMemory = 0, kErrRead = 1, kErrWrite = 2, kErrSeek = 3, kErrUnsupported = 4 };

struct RWops;
struct RWops {
    int64_t (*size)(RWops* ctx);
    int64_t (*seek)(RWops* ctx, int64_t offset, int whence);
    size_t (*read)(RWops* ctx, void* ptr, size_t size, size_t maxnum);
    size_t (*write)(RWops* ctx, const void* ptr, size_t size, size_t num);
    int (*close)(RWops* ctx);
    uint32_t type;
    union {
        struct { void* data1; void* data2; } unknown;
        struct { int autoclose; FILE* fp; } stdio;
        struct { uint8_t* base; uint8_t* here; uint8_t* stop; } mem;
    } hidden;
};
static_assert(sizeof(RWops) == 72, "SDL_RWops");
static_assert(offsetof(RWops, type) == 40, "SDL_RWops.type");

// SDL_Error: set the message of one of the five numbered errors; answer -1.
inline int errorCode(int code) {
    switch (code) {
    case kErrNoMemory: return setError("Out of memory");
    case kErrRead: return setError("Error reading from datastream");
    case kErrWrite: return setError("Error writing to datastream");
    case kErrSeek: return setError("Error seeking in datastream");
    case kErrUnsupported: return setError("That operation is not supported");
    default: return setError("Unknown SDL error");
    }
}

inline RWops* allocRW() {
    auto* r = static_cast<RWops*>(malloc(sizeof(RWops)));
    if (!r) { errorCode(kErrNoMemory); return nullptr; }
    r->type = kTypeUnknown;   // SDL sets only the type; the other fields are the caller's to fill
    return r;
}

inline void freeRW(RWops* r) { free(r); }

// ----- file streams -----

inline int64_t stdioSeek(RWops* c, int64_t offset, int whence) {
    int w;
    switch (whence) {
    case kSeekSet: w = SEEK_SET; break;
    case kSeekCur: w = SEEK_CUR; break;
    case kSeekEnd: w = SEEK_END; break;
    default: return setError("Unknown value for 'whence'");
    }
    if (fseeko(c->hidden.stdio.fp, static_cast<off_t>(offset), w) == 0) {
        const off_t pos = ftello(c->hidden.stdio.fp);
        if (pos < 0) return setError("Couldn't get stream offset");
        return pos;
    }
    return errorCode(kErrSeek);
}

inline int64_t stdioSize(RWops* c) {
    const int64_t pos = stdioSeek(c, 0, kSeekCur);
    if (pos < 0) return -1;
    const int64_t size = stdioSeek(c, 0, kSeekEnd);
    stdioSeek(c, pos, kSeekSet);
    return size;
}

inline size_t stdioRead(RWops* c, void* ptr, size_t size, size_t maxnum) {
    const size_t n = fread(ptr, size, maxnum, c->hidden.stdio.fp);
    if (n == 0 && ferror(c->hidden.stdio.fp)) errorCode(kErrRead);
    return n;
}

inline size_t stdioWrite(RWops* c, const void* ptr, size_t size, size_t num) {
    const size_t n = fwrite(ptr, size, num, c->hidden.stdio.fp);
    if (n == 0 && ferror(c->hidden.stdio.fp)) errorCode(kErrWrite);
    return n;
}

inline int stdioClose(RWops* c) {
    int status = 0;
    if (c) {
        if (c->hidden.stdio.autoclose && fclose(c->hidden.stdio.fp) != 0) status = errorCode(kErrWrite);
        freeRW(c);
    }
    return status;
}

inline RWops* fromFP(FILE* fp, int autoclose) {
    RWops* r = allocRW();
    if (!r) return nullptr;
    r->size = stdioSize;
    r->seek = stdioSeek;
    r->read = stdioRead;
    r->write = stdioWrite;
    r->close = stdioClose;
    r->hidden.stdio.fp = fp;
    r->hidden.stdio.autoclose = autoclose;
    r->type = kTypeStdFile;
    return r;
}

inline RWops* fromFile(const char* file, const char* mode) {
    if (!file || !*file || !mode || !*mode) {
        setError("SDL_RWFromFile(): No file or no mode specified");
        return nullptr;
    }
    FILE* fp = fopen(file, mode);
    if (!fp) {
        // The text follows SDL 2.32, which adds the reason of the C library.
        setError((std::string("Couldn't open ") + file + ": " + strerror(errno)).c_str());
        return nullptr;
    }
    return fromFP(fp, 1);
}

// ----- memory streams -----

inline int64_t memSize(RWops* c) { return c->hidden.mem.stop - c->hidden.mem.base; }

inline int64_t memSeek(RWops* c, int64_t offset, int whence) {
    uint8_t* pos;
    switch (whence) {
    case kSeekSet: pos = c->hidden.mem.base + offset; break;
    case kSeekCur: pos = c->hidden.mem.here + offset; break;
    case kSeekEnd: pos = c->hidden.mem.stop + offset; break;
    default: return setError("Unknown value for 'whence'");
    }
    if (pos < c->hidden.mem.base) pos = c->hidden.mem.base;
    if (pos > c->hidden.mem.stop) pos = c->hidden.mem.stop;
    c->hidden.mem.here = pos;
    return c->hidden.mem.here - c->hidden.mem.base;
}

inline size_t memRead(RWops* c, void* ptr, size_t size, size_t maxnum) {
    size_t total = maxnum * size;
    if (!maxnum || !size || (total / maxnum) != size) return 0;
    const size_t available = static_cast<size_t>(c->hidden.mem.stop - c->hidden.mem.here);
    if (total > available) total = available;
    memcpy(ptr, c->hidden.mem.here, total);
    c->hidden.mem.here += total;
    return total / size;
}

inline size_t memWrite(RWops* c, const void* ptr, size_t size, size_t num) {
    if (size == 0) return 0;
    if (static_cast<size_t>(c->hidden.mem.stop - c->hidden.mem.here) < num * size)
        num = static_cast<size_t>(c->hidden.mem.stop - c->hidden.mem.here) / size;
    memcpy(c->hidden.mem.here, ptr, num * size);
    c->hidden.mem.here += num * size;
    return num;
}

inline size_t memWriteConst(RWops*, const void*, size_t, size_t) {
    setError("Can't write to read-only memory");
    return 0;
}

inline int memClose(RWops* c) {
    if (c) freeRW(c);
    return 0;
}

inline RWops* fromMem(void* mem, int size) {
    if (!mem) { invalidParam("mem"); return nullptr; }
    if (!size) { invalidParam("size"); return nullptr; }
    RWops* r = allocRW();
    if (!r) return nullptr;
    r->size = memSize;
    r->seek = memSeek;
    r->read = memRead;
    r->write = memWrite;
    r->close = memClose;
    r->hidden.mem.base = static_cast<uint8_t*>(mem);
    r->hidden.mem.here = r->hidden.mem.base;
    r->hidden.mem.stop = r->hidden.mem.base + size;
    r->type = kTypeMemory;
    return r;
}

inline RWops* fromConstMem(const void* mem, int size) {
    RWops* r = fromMem(const_cast<void*>(mem), size);
    if (r) {
        r->write = memWriteConst;
        r->type = kTypeMemoryReadOnly;
    }
    return r;
}

// ----- wrappers that only call the function pointer of the stream -----

inline int64_t rwSize(RWops* c) { return c->size(c); }
inline int64_t rwSeek(RWops* c, int64_t offset, int whence) { return c->seek(c, offset, whence); }
inline int64_t rwTell(RWops* c) { return c->seek(c, 0, kSeekCur); }
inline size_t rwRead(RWops* c, void* p, size_t size, size_t n) { return c->read(c, p, size, n); }
inline size_t rwWrite(RWops* c, const void* p, size_t size, size_t n) { return c->write(c, p, size, n); }
inline int rwClose(RWops* c) { return c->close(c); }

// SDL_LoadFile_RW: the whole stream in a malloc block with a zero byte after it.
inline void* loadFileRW(RWops* src, size_t* datasize, int freesrc) {
    if (!src) { invalidParam("src"); return nullptr; }
    constexpr int64_t kChunk = 1024;
    int64_t size = rwSize(src);
    if (size < 0) size = kChunk;
    char* data = static_cast<char*>(malloc(static_cast<size_t>(size + 1)));
    size_t total = 0;
    for (;;) {
        if (static_cast<int64_t>(total) + kChunk > size) {
            size = static_cast<int64_t>(total) + kChunk;
            char* bigger = static_cast<char*>(realloc(data, static_cast<size_t>(size + 1)));
            if (!bigger) {
                free(data);
                data = nullptr;
                errorCode(kErrNoMemory);
                break;
            }
            data = bigger;
        }
        const size_t got = rwRead(src, data + total, 1, static_cast<size_t>(size) - total);
        if (got == 0) break;
        total += got;
    }
    if (data) {
        if (datasize) *datasize = total;
        data[total] = '\0';
    }
    if (freesrc) rwClose(src);
    return data;
}

inline void* loadFile(const char* file, size_t* datasize) {
    return loadFileRW(fromFile(file, "rb"), datasize, 1);
}

// ----- endian helpers -----

inline uint16_t swap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }
inline uint32_t swap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t swap64(uint64_t v) { return __builtin_bswap64(v); }

// The target is little endian on every supported platform (arm64, x86_64).
inline uint8_t readU8(RWops* s) { uint8_t v = 0; rwRead(s, &v, sizeof(v), 1); return v; }
inline uint16_t readLE16(RWops* s) { uint16_t v = 0; rwRead(s, &v, sizeof(v), 1); return v; }
inline uint16_t readBE16(RWops* s) { return swap16(readLE16(s)); }
inline uint32_t readLE32(RWops* s) { uint32_t v = 0; rwRead(s, &v, sizeof(v), 1); return v; }
inline uint32_t readBE32(RWops* s) { return swap32(readLE32(s)); }
inline uint64_t readLE64(RWops* s) { uint64_t v = 0; rwRead(s, &v, sizeof(v), 1); return v; }
inline uint64_t readBE64(RWops* s) { return swap64(readLE64(s)); }
inline size_t writeU8(RWops* d, uint8_t v) { return rwWrite(d, &v, sizeof(v), 1); }
inline size_t writeLE16(RWops* d, uint16_t v) { return rwWrite(d, &v, sizeof(v), 1); }
inline size_t writeBE16(RWops* d, uint16_t v) { v = swap16(v); return rwWrite(d, &v, sizeof(v), 1); }
inline size_t writeLE32(RWops* d, uint32_t v) { return rwWrite(d, &v, sizeof(v), 1); }
inline size_t writeBE32(RWops* d, uint32_t v) { v = swap32(v); return rwWrite(d, &v, sizeof(v), 1); }
inline size_t writeLE64(RWops* d, uint64_t v) { return rwWrite(d, &v, sizeof(v), 1); }
inline size_t writeBE64(RWops* d, uint64_t v) { v = swap64(v); return rwWrite(d, &v, sizeof(v), 1); }

} // namespace rw
} // namespace sdl2
} // namespace pharo
