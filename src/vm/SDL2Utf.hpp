/*
 * SDL2Utf.hpp - named constants and small helpers for UTF-8 and UTF-16 text,
 * shared by the SDL2 stub files.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace pharo {
namespace sdl2 {
namespace utf {

// Code point limits.
constexpr uint32_t kOneByteLimit = 0x80;        // below: one UTF-8 byte
constexpr uint32_t kTwoByteLimit = 0x800;       // below: two UTF-8 bytes
constexpr uint32_t kBmpLimit = 0x10000;         // below: three UTF-8 bytes, one UTF-16 unit
constexpr uint32_t kMaxCodePoint = 0x10FFFF;
constexpr uint32_t kLatin1Limit = 0x100;
constexpr uint32_t kReplacement = 0xFFFD;       // U+FFFD REPLACEMENT CHARACTER
constexpr uint32_t kByteOrderMark = 0xFEFF;
constexpr uint32_t kHighSurrogateFirst = 0xD800;
constexpr uint32_t kLowSurrogateFirst = 0xDC00;
constexpr uint32_t kLowSurrogateLast = 0xDFFF;
constexpr uint32_t kSurrogateBits = 10;
constexpr uint32_t kSurrogateMask = 0x3FF;

// UTF-8 byte patterns: a lead byte carries the length, a continuation byte has 10 in its top bits.
constexpr uint8_t kContinuationMask = 0xC0;
constexpr uint8_t kContinuationTag = 0x80;
constexpr uint8_t kPayloadMask = 0x3F;
constexpr uint8_t kLead2Tag = 0xC0, kLead3Tag = 0xE0, kLead4Tag = 0xF0, kLead5Tag = 0xF8;
constexpr uint8_t kLead2Payload = 0x1F, kLead3Payload = 0x0F, kLead4Payload = 0x07;
constexpr unsigned kPayloadBits = 6;

inline bool isContinuation(unsigned char b) { return (b & kContinuationMask) == kContinuationTag; }

// The length of the sequence that starts with `lead`; 0 for a byte that cannot start one.
inline size_t sequenceLength(unsigned char lead) {
    if (lead < kOneByteLimit) return 1;
    if (lead >= kLead4Tag) return lead < kLead5Tag ? 4 : 0;
    if (lead >= kLead3Tag) return 3;
    if (lead >= kLead2Tag) return 2;
    return 0;
}

// Write the UTF-8 form of `cp` and a zero byte; answers the byte count of the character.
inline size_t encode(uint32_t cp, char* out) {
    size_t n;
    if (cp < kOneByteLimit) { out[0] = static_cast<char>(cp); n = 1; }
    else if (cp < kTwoByteLimit) {
        out[0] = static_cast<char>(kLead2Tag | (cp >> kPayloadBits));
        out[1] = static_cast<char>(kContinuationTag | (cp & kPayloadMask));
        n = 2;
    } else if (cp < kBmpLimit) {
        out[0] = static_cast<char>(kLead3Tag | (cp >> (2 * kPayloadBits)));
        out[1] = static_cast<char>(kContinuationTag | ((cp >> kPayloadBits) & kPayloadMask));
        out[2] = static_cast<char>(kContinuationTag | (cp & kPayloadMask));
        n = 3;
    } else {
        out[0] = static_cast<char>(kLead4Tag | (cp >> (3 * kPayloadBits)));
        out[1] = static_cast<char>(kContinuationTag | ((cp >> (2 * kPayloadBits)) & kPayloadMask));
        out[2] = static_cast<char>(kContinuationTag | ((cp >> kPayloadBits) & kPayloadMask));
        out[3] = static_cast<char>(kContinuationTag | (cp & kPayloadMask));
        n = 4;
    }
    out[n] = '\0';
    return n;
}

// The payload bits of the lead byte of a sequence of `length` bytes.
inline uint32_t leadPayload(unsigned char lead, size_t length) {
    switch (length) {
    case 2: return lead & kLead2Payload;
    case 3: return lead & kLead3Payload;
    default: return lead & kLead4Payload;
    }
}

} // namespace utf
} // namespace sdl2
} // namespace pharo
