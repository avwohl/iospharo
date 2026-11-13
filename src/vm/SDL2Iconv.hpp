/*
 * SDL2Iconv.hpp - SDL_iconv_open, SDL_iconv, SDL_iconv_close and
 * SDL_iconv_string: text conversion between the character sets that SDL
 * knows (ASCII, ISO-8859-1, UTF-8, UTF-16, UTF-32, UCS-2, UCS-4).
 *
 * The behaviour follows a real libSDL2: a character that the target set
 * cannot hold becomes '?', an invalid UTF-8 byte becomes U+FFFD, UTF-16
 * output starts with a byte order mark, the plain names UTF-16 and UTF-32
 * are little endian and UCS-2 and UCS-4 are big endian.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Sys.hpp"
#include "SDL2Utf.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace pharo {
namespace sdl2 {
namespace iconvlib {

constexpr size_t kError = static_cast<size_t>(-1), kE2Big = static_cast<size_t>(-2),
                 kEIlSeq = static_cast<size_t>(-3), kEInval = static_cast<size_t>(-4);
using utf::kReplacement;
constexpr uint32_t kBom = utf::kByteOrderMark, kMaxCodePoint = utf::kMaxCodePoint;
constexpr uint32_t kAsciiLimit = utf::kOneByteLimit, kLatin1Limit = utf::kLatin1Limit;

enum Kind { kAscii, kLatin1, kUtf8, kUtf16, kUtf32, kUcs2, kUcs4 };
struct Codec { const char* name; Kind kind; bool big; bool bom; };

inline const Codec* findCodec(const char* name) {
    if (!name || !*name) name = "UTF-8";   // SDL takes a null or empty name as UTF-8
    static const Codec table[] = {
        { "ASCII", kAscii, false, false }, { "ISO-8859-1", kLatin1, false, false }, { "UTF-8", kUtf8, false, false },
        { "UTF-16", kUtf16, false, true }, { "UTF-16LE", kUtf16, false, false }, { "UTF-16BE", kUtf16, true, false },
        { "UTF-32", kUtf32, false, false }, { "UTF-32LE", kUtf32, false, false }, { "UTF-32BE", kUtf32, true, false },
        { "UCS-2", kUcs2, true, false }, { "UCS-2LE", kUcs2, false, false }, { "UCS-2BE", kUcs2, true, false },
        { "UCS-4", kUcs4, true, false }, { "UCS-4LE", kUcs4, false, false }, { "UCS-4BE", kUcs4, true, false },
        { "WCHAR_T", kUcs4, false, false }, { "UCS-2-INTERNAL", kUcs2, false, false }, { "UCS-4-INTERNAL", kUcs4, false, false },
    };
    for (const Codec& c : table)
        if (name && strcasecmp(name, c.name) == 0) return &c;
    return nullptr;
}

struct Context {
    const Codec* to;
    const Codec* from;
    bool bomWritten = false;
};

inline Context* open(const char* to, const char* from) {
    const Codec* t = findCodec(to);
    const Codec* f = findCodec(from);
    if (!t || !f) return nullptr;
    return new Context{ t, f };
}

// Decode one character from [in, in+left).  Answers its length in bytes, 0 when the input ends inside it.
inline size_t decode(const Codec& c, const uint8_t* in, size_t left, uint32_t& cp) {
    if (left == 0) return 0;
    switch (c.kind) {
    case kAscii: cp = in[0] < kAsciiLimit ? in[0] : kReplacement; return 1;
    case kLatin1: cp = in[0]; return 1;
    case kUtf8: {
        const uint8_t b = in[0];
        const size_t n = utf::sequenceLength(b);
        if (n == 0) { cp = kReplacement; return 1; }
        if (left < n) return 0;
        cp = n == 1 ? b : utf::leadPayload(b, n);
        for (size_t i = 1; i < n; i++) {
            if (!utf::isContinuation(in[i])) { cp = kReplacement; return i; }
            cp = (cp << utf::kPayloadBits) | (in[i] & utf::kPayloadMask);
        }
        if (cp > kMaxCodePoint) cp = kReplacement;
        return n;
    }
    case kUtf16: case kUcs2: {
        if (left < 2) return 0;
        const uint32_t u = c.big ? (in[0] << 8 | in[1]) : (in[1] << 8 | in[0]);
        if (c.kind == kUtf16 && u >= utf::kHighSurrogateFirst && u < utf::kLowSurrogateFirst) {
            if (left < 4) return 0;
            const uint32_t v = c.big ? (in[2] << 8 | in[3]) : (in[3] << 8 | in[2]);
            if (v < utf::kLowSurrogateFirst || v > utf::kLowSurrogateLast) { cp = kReplacement; return 2; }
            cp = utf::kBmpLimit + ((u - utf::kHighSurrogateFirst) << utf::kSurrogateBits) + (v - utf::kLowSurrogateFirst);
            return 4;
        }
        cp = u;
        return 2;
    }
    default: {
        if (left < 4) return 0;
        cp = c.big ? (static_cast<uint32_t>(in[0]) << 24 | in[1] << 16 | in[2] << 8 | in[3])
                   : (static_cast<uint32_t>(in[3]) << 24 | in[2] << 16 | in[1] << 8 | in[0]);
        if (cp > kMaxCodePoint) cp = kReplacement;
        return 4;
    }
    }
}

// Encode one character into `out` (room `left`).  Answers the byte count, 0 when it does not fit.
inline size_t encode(const Codec& c, uint32_t cp, uint8_t* out, size_t left) {
    auto put16 = [&](uint32_t u, uint8_t* o) { if (c.big) { o[0] = u >> 8; o[1] = u & 255; } else { o[0] = u & 255; o[1] = u >> 8; } };
    switch (c.kind) {
    case kAscii: if (left < 1) return 0; out[0] = cp < kAsciiLimit ? static_cast<uint8_t>(cp) : '?'; return 1;
    case kLatin1: if (left < 1) return 0; out[0] = cp < kLatin1Limit ? static_cast<uint8_t>(cp) : '?'; return 1;
    case kUtf8: {
        char buf[5];
        const size_t n = utf::encode(cp, buf);
        if (left < n) return 0;
        memcpy(out, buf, n);
        return n;
    }
    case kUtf16: {
        if (cp >= utf::kBmpLimit) {
            if (left < 4) return 0;
            const uint32_t v = cp - utf::kBmpLimit;
            put16(utf::kHighSurrogateFirst + (v >> utf::kSurrogateBits), out);
            put16(utf::kLowSurrogateFirst + (v & utf::kSurrogateMask), out + 2);
            return 4;
        }
        if (left < 2) return 0;
        put16(cp, out);
        return 2;
    }
    case kUcs2: if (left < 2) return 0; put16(cp < utf::kBmpLimit ? cp : kReplacement, out); return 2;
    default:
        if (left < 4) return 0;
        if (c.big) { out[0] = cp >> 24; out[1] = (cp >> 16) & 255; out[2] = (cp >> 8) & 255; out[3] = cp & 255; }
        else { out[3] = cp >> 24; out[2] = (cp >> 16) & 255; out[1] = (cp >> 8) & 255; out[0] = cp & 255; }
        return 4;
    }
}

// SDL_iconv: convert as much as fits.  Answers the number of irreversible conversions (0 here),
// or one of the error values.
inline size_t convert(Context* cd, const char** inbuf, size_t* inleft, char** outbuf, size_t* outleft) {
    if (!cd || !inbuf || !*inbuf || !inleft) {
        if (outbuf && *outbuf && outleft && cd && cd->to->bom && !cd->bomWritten) { /* reset state: nothing to flush */ }
        return 0;
    }
    const uint8_t* in = reinterpret_cast<const uint8_t*>(*inbuf);
    uint8_t* out = reinterpret_cast<uint8_t*>(*outbuf);
    size_t result = 0;
    while (*inleft > 0) {
        uint32_t cp;
        const size_t n = decode(*cd->from, in, *inleft, cp);
        if (n == 0) { result = kEInval; break; }
        if (cd->to->bom && !cd->bomWritten) {
            const size_t b = encode(*cd->to, kBom, out, *outleft);
            if (b == 0) { result = kE2Big; break; }
            out += b; *outleft -= b;
            cd->bomWritten = true;
        }
        const size_t m = encode(*cd->to, cp, out, *outleft);
        if (m == 0) { result = kE2Big; break; }
        in += n; *inleft -= n;
        out += m; *outleft -= m;
    }
    *inbuf = reinterpret_cast<const char*>(in);
    *outbuf = reinterpret_cast<char*>(out);
    return result;
}

// SDL_iconv_string: the whole input in a new block with a zero character after it.
inline char* convertString(const char* to, const char* from, const char* in, size_t inLeft,
                           void* (*alloc)(size_t), void* (*grow)(void*, size_t), void (*release)(void*)) {
    Context* cd = open(to, from);
    if (!cd) return nullptr;
    size_t capacity = (inLeft + 1) * 4, used = 0;
    char* out = static_cast<char*>(alloc(capacity));
    if (!out) { delete cd; return nullptr; }
    const size_t terminator = cd->to->kind == kUtf16 || cd->to->kind == kUcs2 ? 2 : (cd->to->kind == kUtf32 || cd->to->kind == kUcs4 ? 4 : 1);
    while (true) {
        char* o = out + used;
        size_t room = capacity - used - terminator;
        const char* ip = in;
        size_t il = inLeft;
        const size_t r = convert(cd, &ip, &il, &o, &room);
        used = static_cast<size_t>(o - out);
        in = ip;
        inLeft = il;
        if (r == kE2Big) {
            capacity *= 2;
            char* bigger = static_cast<char*>(grow(out, capacity));
            if (!bigger) { release(out); delete cd; return nullptr; }
            out = bigger;
            continue;
        }
        if (r == kEInval || r == kError || r == kEIlSeq) { release(out); delete cd; return nullptr; }
        break;
    }
    memset(out + used, 0, terminator);
    delete cd;
    return out;
}

} // namespace iconvlib
} // namespace sdl2
} // namespace pharo
