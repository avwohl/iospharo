/*
 * SDL2Audio.hpp - the audio calls of SDL_audio.h that need no audio device:
 * audio streams, audio converters (SDL_AudioCVT), SDL_MixAudioFormat and WAV
 * loading.
 *
 * The conversion decodes the input to floating point samples, maps the
 * channels, resamples with linear interpolation and encodes the output.  The
 * channel rules are the simple ones (mono is copied to every channel, a mix to
 * mono is the average, a mix of more channels to fewer takes the first
 * channels), so the output is close to SDL's but not bit for bit the same
 * when the rate or the channel count changes.  A conversion that only changes
 * the sample format is exact.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2RW.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pharo {
namespace sdl2 {
namespace audio {

using sys::setError;
using sys::invalidParam;

// SDL_AudioFormat: the low byte is the bit size, bit 15 marks signed, bit 12
// big endian and bit 8 floating point.
constexpr uint16_t kFlagSigned = 1u << 15, kFlagBigEndian = 1u << 12, kFlagFloat = 1u << 8;
constexpr uint16_t kU8 = 0x0008, kS8 = 0x8008, kU16Lsb = 0x0010, kS16Lsb = 0x8010, kU16Msb = 0x1010,
                   kS16Msb = 0x9010, kS32Lsb = 0x8020, kS32Msb = 0x9020, kF32Lsb = 0x8120, kF32Msb = 0x9120;
constexpr int kMixMaxVolume = 128;
constexpr uint8_t kSilenceU8 = 128;   // the middle of the unsigned 8-bit range

inline bool knownFormat(uint16_t f) {
    return f == kU8 || f == kS8 || f == kU16Lsb || f == kS16Lsb || f == kU16Msb || f == kS16Msb ||
           f == kS32Lsb || f == kS32Msb || f == kF32Lsb || f == kF32Msb;
}
constexpr uint16_t kBitSizeMask = 0xFF;   // the low byte of an audio format is the sample size in bits
inline int sampleBytes(uint16_t f) { return (f & kBitSizeMask) / 8; }
inline bool validChannels(int c) { return c >= 1 && c <= 8; }
inline bool isFloatFormat(uint16_t f) { return (f & kFlagFloat) != 0; }

// The scale between a sample of an integer format and the range -1..1.
inline double integerScale(uint16_t f) {
    switch (sampleBytes(f)) {
    case 1: return 128.0;
    case 2: return 32768.0;
    default: return 2147483648.0;
    }
}

inline double decodeSample(const uint8_t* p, uint16_t f) {
    const bool big = (f & kFlagBigEndian) != 0;
    switch (f) {
    case kU8: return (static_cast<int>(p[0]) - 128) / 128.0;
    case kS8: return static_cast<int8_t>(p[0]) / 128.0;
    case kU16Lsb: case kU16Msb: { const uint16_t v = big ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]); return (static_cast<int>(v) - 32768) / 32768.0; }
    case kS16Lsb: case kS16Msb: { const int16_t v = static_cast<int16_t>(big ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0])); return v / 32768.0; }
    case kS32Lsb: case kS32Msb: {
        const uint32_t u = big ? (static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]) : (static_cast<uint32_t>(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0]);
        return static_cast<int32_t>(u) / 2147483648.0;
    }
    default: {   // F32
        uint32_t u = big ? (static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]) : (static_cast<uint32_t>(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0]);
        float v;
        memcpy(&v, &u, 4);
        return v;
    }
    }
}

// An integer format takes the sample times the scale of its range, rounded half up and
// clamped; a float format takes the sample as it is.  A widening conversion
// (S16 to S32, U8 to S16) is then an exact shift, as it is in SDL.
inline void encodeSample(uint8_t* p, uint16_t f, double v) {
    const bool big = (f & kFlagBigEndian) != 0;
    auto put32 = [&](uint32_t u) {
        if (big) { p[0] = u >> 24; p[1] = (u >> 16) & 255; p[2] = (u >> 8) & 255; p[3] = u & 255; }
        else { p[3] = u >> 24; p[2] = (u >> 16) & 255; p[1] = (u >> 8) & 255; p[0] = u & 255; }
    };
    auto put16 = [&](uint32_t u) { if (big) { p[0] = u >> 8; p[1] = u & 255; } else { p[0] = u & 255; p[1] = u >> 8; } };
    if (isFloatFormat(f)) {
        const float fv = static_cast<float>(v);
        uint32_t u;
        memcpy(&u, &fv, 4);
        put32(u);
        return;
    }
    const double scale = integerScale(f);
    double x = std::floor(v * scale + 0.5);
    const double lo = -scale, hi = scale - 1.0;
    x = std::max(lo, std::min(hi, x));
    const int64_t n = static_cast<int64_t>(x);
    switch (f) {
    case kU8: p[0] = static_cast<uint8_t>(n + 128); break;
    case kS8: p[0] = static_cast<uint8_t>(static_cast<int8_t>(n)); break;
    case kU16Lsb: case kU16Msb: put16(static_cast<uint32_t>(n + 32768)); break;
    case kS16Lsb: case kS16Msb: put16(static_cast<uint16_t>(static_cast<int16_t>(n))); break;
    default: put32(static_cast<uint32_t>(static_cast<int32_t>(n))); break;
    }
}

struct Converter {
    uint16_t srcFormat = 0, dstFormat = 0;
    int srcChannels = 0, dstChannels = 0, srcRate = 0, dstRate = 0;
    // Resampling state: the last input frame and the position of the next
    // output frame, in input frames, relative to that frame.
    std::vector<double> last;
    double position = 0.0;
    bool haveLast = false;

    bool needed() const { return srcFormat != dstFormat || srcChannels != dstChannels || srcRate != dstRate; }

    // Convert whole frames of `in`; append the output frames to `out`.
    void convert(const uint8_t* in, size_t inBytes, std::vector<uint8_t>& out) {
        const size_t frameBytes = static_cast<size_t>(sampleBytes(srcFormat)) * srcChannels;
        const size_t frames = inBytes / frameBytes;
        std::vector<double> mixed(frames * dstChannels);
        for (size_t i = 0; i < frames; i++) {
            double src[8];
            for (int c = 0; c < srcChannels; c++) src[c] = decodeSample(in + i * frameBytes + static_cast<size_t>(c) * sampleBytes(srcFormat), srcFormat);
            double* d = &mixed[i * dstChannels];
            if (srcChannels == dstChannels) { for (int c = 0; c < dstChannels; c++) d[c] = src[c]; }
            else if (srcChannels == 1) { for (int c = 0; c < dstChannels; c++) d[c] = src[0]; }
            else if (dstChannels == 1) { double s = 0; for (int c = 0; c < srcChannels; c++) s += src[c]; d[0] = s / srcChannels; }
            else { for (int c = 0; c < dstChannels; c++) d[c] = c < srcChannels ? src[c] : 0.0; }
        }
        std::vector<double> resampled;
        if (srcRate == dstRate) {
            resampled = std::move(mixed);
        } else {
            const double step = static_cast<double>(srcRate) / dstRate;   // input frames per output frame
            std::vector<double> all;
            if (haveLast) all.insert(all.end(), last.begin(), last.end());
            all.insert(all.end(), mixed.begin(), mixed.end());
            const size_t total = all.size() / dstChannels;
            while (position + 1.0 <= static_cast<double>(total) - 1.0 + 1e-9 && total >= 2) {
                const size_t i0 = static_cast<size_t>(position);
                const double frac = position - static_cast<double>(i0);
                for (int c = 0; c < dstChannels; c++) {
                    const double a = all[i0 * dstChannels + c], b = all[(i0 + 1) * dstChannels + c];
                    resampled.push_back(a + (b - a) * frac);
                }
                position += step;
            }
            if (total >= 1) {
                // Keep the frame that the next output frame still needs.
                const size_t keep = std::min(static_cast<size_t>(position), total - 1);
                last.assign(all.begin() + static_cast<std::ptrdiff_t>(keep * dstChannels), all.end());
                position -= static_cast<double>(keep);
                haveLast = true;
            }
        }
        const size_t outFrameBytes = static_cast<size_t>(sampleBytes(dstFormat)) * dstChannels;
        const size_t outFrames = resampled.size() / dstChannels;
        const size_t base = out.size();
        out.resize(base + outFrames * outFrameBytes);
        for (size_t i = 0; i < outFrames; i++)
            for (int c = 0; c < dstChannels; c++)
                encodeSample(&out[base + i * outFrameBytes + static_cast<size_t>(c) * sampleBytes(dstFormat)], dstFormat, resampled[i * dstChannels + c]);
    }
};

// ----- audio streams -----

struct Stream {
    Converter conv;
    std::vector<uint8_t> queue;     // converted bytes not yet read
    std::mutex mx;
    size_t srcFrameBytes = 0;
};

inline Stream* newStream(uint16_t sf, int sc, int sr, uint16_t df, int dc, int dr) {
    if (!validChannels(sc)) { invalidParam("src_channels"); return nullptr; }
    if (!validChannels(dc)) { invalidParam("dst_channels"); return nullptr; }
    if (sr <= 0) { invalidParam("src_rate"); return nullptr; }
    if (dr <= 0) { invalidParam("dst_rate"); return nullptr; }
    if (!knownFormat(sf)) { setError("Invalid source format"); return nullptr; }
    if (!knownFormat(df)) { setError("Invalid destination format"); return nullptr; }
    auto* s = new Stream;
    s->conv.srcFormat = sf; s->conv.dstFormat = df; s->conv.srcChannels = sc; s->conv.dstChannels = dc;
    s->conv.srcRate = sr; s->conv.dstRate = dr;
    s->srcFrameBytes = static_cast<size_t>(sampleBytes(sf)) * sc;
    return s;
}

inline int streamPut(Stream* s, const void* buf, int len) {
    if (!s) return invalidParam("stream");
    if (!buf) return invalidParam("buf");
    if (len < 0) return invalidParam("len");
    if (len == 0) return 0;
    if (static_cast<size_t>(len) % s->srcFrameBytes != 0) return setError("Can't add partial sample frames");
    std::lock_guard<std::mutex> lk(s->mx);
    s->conv.convert(static_cast<const uint8_t*>(buf), static_cast<size_t>(len), s->queue);
    return 0;
}

inline int streamGet(Stream* s, void* buf, int len) {
    if (!s) return invalidParam("stream");
    if (!buf) return invalidParam("buf");
    if (len < 0) return invalidParam("len");
    std::lock_guard<std::mutex> lk(s->mx);
    const size_t n = std::min(static_cast<size_t>(len), s->queue.size());
    memcpy(buf, s->queue.data(), n);
    s->queue.erase(s->queue.begin(), s->queue.begin() + static_cast<std::ptrdiff_t>(n));
    return static_cast<int>(n);
}

inline int streamAvailable(Stream* s) {
    if (!s) return 0;
    std::lock_guard<std::mutex> lk(s->mx);
    return static_cast<int>(s->queue.size());
}

inline int streamFlush(Stream* s) {
    if (!s) return invalidParam("stream");
    std::lock_guard<std::mutex> lk(s->mx);
    s->conv.haveLast = false;
    s->conv.last.clear();
    s->conv.position = 0.0;
    return 0;
}

inline void streamClear(Stream* s) {
    if (!s) { invalidParam("stream"); return; }
    std::lock_guard<std::mutex> lk(s->mx);
    s->queue.clear();
    s->conv.haveLast = false;
    s->conv.last.clear();
    s->conv.position = 0.0;
}

// ----- SDL_AudioCVT -----

#pragma pack(push, 1)
struct Cvt {                          // SDL_AudioCVT, packed as in SDL
    int needed;
    uint16_t src_format;
    uint16_t dst_format;
    double rate_incr;
    uint8_t* buf;
    int len;
    int len_cvt;
    int len_mult;
    double len_ratio;
    void* filters[10];
    int filter_index;
};
#pragma pack(pop)

// SDL_BuildAudioCVT: 1 when a conversion is needed, 0 when not, -1 on error.
inline int buildCvt(Cvt* cvt, uint16_t sf, int sc, int sr, uint16_t df, int dc, int dr) {
    if (!cvt) return invalidParam("cvt");
    if (!knownFormat(sf)) return setError("Invalid source format");
    if (!knownFormat(df)) return setError("Invalid destination format");
    if (!validChannels(sc)) return setError("Invalid source channels");
    if (!validChannels(dc)) return setError("Invalid destination channels");
    if (sr <= 0) return setError("Source rate is equal to or less than zero");
    if (dr <= 0) return setError("Destination rate is equal to or less than zero");
    memset(cvt, 0, sizeof(*cvt));
    cvt->src_format = sf;
    cvt->dst_format = df;
    cvt->len_mult = 1;
    cvt->len_ratio = 1.0;
    cvt->rate_incr = 0.0;
    const bool need = sf != df || sc != dc || sr != dr;
    if (!need) return 0;
    // The buffer size and length ratio follow the SDL pipeline: convert to float, change
    // the channels, resample, convert to the destination format.
    int mult = 1;
    double ratio = 1.0;
    const int srcBits = sampleBytes(sf) * 8, dstBits = sampleBytes(df) * 8;
    const bool byteswapOnly = sr == dr && sc == dc && (sf & ~kFlagBigEndian) == (df & ~kFlagBigEndian);
    if (!byteswapOnly) {
        if (!isFloatFormat(sf)) {
            const int fbits = 32;
            if (srcBits < fbits) { mult *= fbits / srcBits; ratio *= fbits / srcBits; }
            else if (srcBits > fbits) ratio /= srcBits / fbits;
        }
        if (sc != dc) {
            if (sc < dc) mult = (mult * dc + (sc - 1)) / sc;
            ratio = ratio * dc / sc;
        }
        if (sr != dr) {
            if (sr < dr) { const double m = static_cast<double>(dr) / sr; mult *= static_cast<int>(std::ceil(m)); ratio *= m; }
            else ratio /= static_cast<double>(sr) / dr;
            mult *= 2;   // the resampler needs a scratch buffer as large as the result
        }
        if (!isFloatFormat(df)) {
            const int fbits = 32;
            if (dstBits < fbits) ratio /= fbits / dstBits;
            else if (dstBits > fbits) { mult *= dstBits / fbits; ratio *= dstBits / fbits; }
        }
    }
    cvt->len_ratio = ratio;
    cvt->len_mult = mult;
    if (sr != dr) cvt->rate_incr = static_cast<double>(dr) / sr;
    cvt->needed = 1;
    // The converter parameters travel in the filter slots: the source and
    // destination channels and rates, which SDL_ConvertAudio needs again.
    cvt->filters[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(sc));
    cvt->filters[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(dc));
    cvt->filters[2] = reinterpret_cast<void*>(static_cast<uintptr_t>(sr));
    cvt->filters[3] = reinterpret_cast<void*>(static_cast<uintptr_t>(dr));
    return 1;
}

// SDL_ConvertAudio: convert cvt->buf (cvt->len bytes) in place; the buffer has
// room for len * len_mult bytes.
inline int convertAudio(Cvt* cvt) {
    if (!cvt) return invalidParam("cvt");
    if (!cvt->buf) return invalidParam("cvt->buf");
    if (!cvt->needed) { cvt->len_cvt = cvt->len; return 0; }
    Converter conv;
    conv.srcFormat = cvt->src_format; conv.dstFormat = cvt->dst_format;
    conv.srcChannels = static_cast<int>(reinterpret_cast<uintptr_t>(cvt->filters[0]));
    conv.dstChannels = static_cast<int>(reinterpret_cast<uintptr_t>(cvt->filters[1]));
    conv.srcRate = static_cast<int>(reinterpret_cast<uintptr_t>(cvt->filters[2]));
    conv.dstRate = static_cast<int>(reinterpret_cast<uintptr_t>(cvt->filters[3]));
    if (!conv.srcChannels || !conv.dstChannels || !conv.srcRate || !conv.dstRate) return setError("SDL_ConvertAudio: the converter was not built");
    std::vector<uint8_t> out;
    conv.convert(cvt->buf, static_cast<size_t>(cvt->len), out);
    const size_t room = static_cast<size_t>(cvt->len) * static_cast<size_t>(cvt->len_mult);
    const size_t n = std::min(out.size(), room);
    memcpy(cvt->buf, out.data(), n);
    cvt->len_cvt = static_cast<int>(n);
    return 0;
}

// ----- mixing -----

// SDL_MixAudioFormat: dst = clamp(dst + src * volume / 128), per sample.
inline void mixAudioFormat(uint8_t* dst, const uint8_t* src, uint16_t format, uint32_t len, int volume) {
    if (volume == 0) return;
    if (!knownFormat(format)) { setError("SDL_MixAudioFormat(): unknown audio format"); return; }
    if (volume > kMixMaxVolume) volume = kMixMaxVolume;
    const int bytes = sampleBytes(format);
    for (uint32_t i = 0; i + bytes <= len; i += static_cast<uint32_t>(bytes)) {
        const double s = decodeSample(src + i, format), d = decodeSample(dst + i, format);
        encodeSample(dst + i, format, static_cast<float>(d + s * volume / kMixMaxVolume));
    }
}

// ----- WAV files -----

#pragma pack(push, 1)
struct Spec {                         // SDL_AudioSpec
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void* callback;
    void* userdata;
};
#pragma pack(pop)
static_assert(sizeof(Spec) == 32, "SDL_AudioSpec");

constexpr uint16_t kWavPcm = 1, kWavFloat = 3, kWavExtensible = 0xFFFE;
constexpr uint16_t kDefaultSamples = 4096;

inline uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// SDL_LoadWAV_RW: answers the spec, with the sample data in a malloc block.
inline Spec* loadWav(rw::RWops* src, int freesrc, Spec* spec, uint8_t** audioBuf, uint32_t* audioLen,
                     void* (*alloc)(size_t), void (*release)(void*)) {
    auto finish = [&](Spec* r) { if (freesrc && src) rw::rwClose(src); return r; };
    if (!src) return nullptr;
    if (!spec) { invalidParam("spec"); return finish(nullptr); }
    if (!audioBuf) { invalidParam("audio_buf"); return finish(nullptr); }
    if (!audioLen) { invalidParam("audio_len"); return finish(nullptr); }
    uint8_t head[12];
    if (rw::rwRead(src, head, 1, 12) != 12) { setError("Error reading from datastream"); return finish(nullptr); }
    if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) { setError("Could not find RIFF or WAVE identifiers (not a Waveform file)"); return finish(nullptr); }
    bool haveFmt = false;
    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    for (;;) {
        uint8_t chunk[8];
        if (rw::rwRead(src, chunk, 1, 8) != 8) { setError("Data chunk not found"); return finish(nullptr); }
        const uint32_t size = le32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            std::vector<uint8_t> fmt(std::max<uint32_t>(size, 16));
            if (rw::rwRead(src, fmt.data(), 1, size) != size) { setError("Error reading from datastream"); return finish(nullptr); }
            tag = static_cast<uint16_t>(le16(fmt.data()));
            channels = static_cast<uint16_t>(le16(fmt.data() + 2));
            rate = le32(fmt.data() + 4);
            bits = static_cast<uint16_t>(le16(fmt.data() + 14));
            if (tag == kWavExtensible && size >= 26) tag = static_cast<uint16_t>(le16(fmt.data() + 24));
            haveFmt = true;
            if (size & 1) rw::rwSeek(src, 1, rw::kSeekCur);
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!haveFmt) { setError("Data chunk before the format chunk"); return finish(nullptr); }
            uint16_t format = 0;
            if (tag == kWavPcm && bits == 8) format = kU8;
            else if (tag == kWavPcm && bits == 16) format = kS16Lsb;
            else if (tag == kWavPcm && bits == 32) format = kS32Lsb;
            else if (tag == kWavFloat && bits == 32) format = kF32Lsb;
            else { setError("Unknown WAVE data format"); return finish(nullptr); }
            uint8_t* buf = static_cast<uint8_t*>(alloc(size ? size : 1));
            if (!buf) { setError("Out of memory"); return finish(nullptr); }
            if (rw::rwRead(src, buf, 1, size) != size) { release(buf); setError("Error reading from datastream"); return finish(nullptr); }
            memset(spec, 0, sizeof(*spec));
            spec->freq = static_cast<int>(rate);
            spec->format = format;
            spec->channels = static_cast<uint8_t>(channels);
            spec->silence = format == kU8 ? kSilenceU8 : 0;
            spec->samples = kDefaultSamples;
            *audioBuf = buf;
            *audioLen = size;
            return finish(spec);
        } else {
            if (rw::rwSeek(src, size + (size & 1), rw::kSeekCur) < 0) { setError("Data chunk not found"); return finish(nullptr); }
        }
    }
}

} // namespace audio
} // namespace sdl2
} // namespace pharo
