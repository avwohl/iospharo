/*
 * SoundPlugin.cpp - Audio output via Audio Queue Services
 *
 * Architecture:
 *   VM thread -> ring buffer -> AudioQueue callback -> speakers
 *
 * The ring buffer is lock-free (single producer, single consumer)
 * using atomic head/tail indices.
 */

#include "SoundPlugin.h"

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <cstring>

// =====================================================================
// Ring buffer (lock-free SPSC)
// =====================================================================

static constexpr int kRingCapacity = 64 * 1024; // 64KB — ~0.37s at 44100 stereo 16-bit
static uint8_t gRingBuffer[kRingCapacity];
static std::atomic<int> gRingHead{0}; // written by VM thread
static std::atomic<int> gRingTail{0}; // advanced by audio callback

static int ringAvailableWrite() {
    int h = gRingHead.load(std::memory_order_relaxed);
    int t = gRingTail.load(std::memory_order_acquire);
    int used = (h - t + kRingCapacity) % kRingCapacity;
    return kRingCapacity - 1 - used; // leave 1 byte gap to distinguish full from empty
}

static int ringAvailableRead() {
    int h = gRingHead.load(std::memory_order_acquire);
    int t = gRingTail.load(std::memory_order_relaxed);
    return (h - t + kRingCapacity) % kRingCapacity;
}

static int ringWrite(const uint8_t* data, int count) {
    int avail = ringAvailableWrite();
    if (count > avail) count = avail;
    if (count <= 0) return 0;

    int h = gRingHead.load(std::memory_order_relaxed);
    int firstChunk = kRingCapacity - h;
    if (firstChunk > count) firstChunk = count;
    memcpy(gRingBuffer + h, data, firstChunk);
    if (count > firstChunk) {
        memcpy(gRingBuffer, data + firstChunk, count - firstChunk);
    }
    gRingHead.store((h + count) % kRingCapacity, std::memory_order_release);
    return count;
}

static int ringRead(uint8_t* data, int count) {
    int avail = ringAvailableRead();
    if (count > avail) count = avail;
    if (count <= 0) return 0;

    int t = gRingTail.load(std::memory_order_relaxed);
    int firstChunk = kRingCapacity - t;
    if (firstChunk > count) firstChunk = count;
    memcpy(data, gRingBuffer + t, firstChunk);
    if (count > firstChunk) {
        memcpy(data + firstChunk, gRingBuffer, count - firstChunk);
    }
    gRingTail.store((t + count) % kRingCapacity, std::memory_order_release);
    return count;
}

// =====================================================================
// Audio Queue output
// =====================================================================

static constexpr int kNumBuffers = 3;
static constexpr int kBufferByteSize = 4096; // ~23ms at 44100 stereo 16-bit

static AudioQueueRef gAudioQueue = nullptr;
static AudioQueueBufferRef gBuffers[kNumBuffers] = {};
static bool gRunning = false;
static int gSemaIndex = 0;
static float gVolume = 1.0f;

// Forward declaration — we need the VM semaphore signaling function
// This is set by the caller (Primitives.cpp) via a callback
typedef void (*SoundSemaSignalFunc)(int index);
static SoundSemaSignalFunc gSignalFunc = nullptr;

extern "C" void soundSetSignalFunc(SoundSemaSignalFunc fn) {
    gSignalFunc = fn;
}

static void audioQueueCallback(void* /*userData*/, AudioQueueRef aq, AudioQueueBufferRef buf) {
    int byteSize = (int)buf->mAudioDataBytesCapacity;
    int got = ringRead((uint8_t*)buf->mAudioData, byteSize);

    // Fill remainder with silence
    if (got < byteSize) {
        memset((uint8_t*)buf->mAudioData + got, 0, byteSize - got);
    }
    buf->mAudioDataByteSize = byteSize;

    AudioQueueEnqueueBuffer(aq, buf, 0, nullptr);

    // Signal VM semaphore that buffer space is available
    if (gSemaIndex > 0 && gSignalFunc) {
        gSignalFunc(gSemaIndex);
    }
}

// =====================================================================
// Public API
// =====================================================================

bool soundInit(int sampleRate, bool stereo, int semaIndex) {
    // Stop existing output first
    if (gRunning) soundStop();

    gSemaIndex = semaIndex;

    // Reset ring buffer
    gRingHead.store(0, std::memory_order_relaxed);
    gRingTail.store(0, std::memory_order_relaxed);

    // Setup audio format: 16-bit signed integer PCM
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = (Float64)sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mChannelsPerFrame = stereo ? 2 : 1;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = fmt.mChannelsPerFrame * 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    OSStatus status = AudioQueueNewOutput(&fmt, audioQueueCallback, nullptr,
                                          nullptr, kCFRunLoopCommonModes, 0,
                                          &gAudioQueue);
    if (status != noErr) return false;

    // Allocate and prime buffers
    for (int i = 0; i < kNumBuffers; i++) {
        status = AudioQueueAllocateBuffer(gAudioQueue, kBufferByteSize, &gBuffers[i]);
        if (status != noErr) {
            AudioQueueDispose(gAudioQueue, true);
            gAudioQueue = nullptr;
            return false;
        }
        // Fill with silence and enqueue
        memset(gBuffers[i]->mAudioData, 0, kBufferByteSize);
        gBuffers[i]->mAudioDataByteSize = kBufferByteSize;
        AudioQueueEnqueueBuffer(gAudioQueue, gBuffers[i], 0, nullptr);
    }

    // Set volume
    AudioQueueSetParameter(gAudioQueue, kAudioQueueParam_Volume, gVolume);

    status = AudioQueueStart(gAudioQueue, nullptr);
    if (status != noErr) {
        AudioQueueDispose(gAudioQueue, true);
        gAudioQueue = nullptr;
        return false;
    }

    gRunning = true;
    return true;
}

void soundStop(void) {
    if (gAudioQueue) {
        AudioQueueStop(gAudioQueue, true);
        AudioQueueDispose(gAudioQueue, true);
        gAudioQueue = nullptr;
    }
    gRunning = false;
    gSemaIndex = 0;
}

int soundAvailableSpace(void) {
    if (!gRunning) return 0;
    return ringAvailableWrite();
}

int soundPlaySamples(const void* samples, int startIndex, int count) {
    if (!gRunning || count <= 0) return 0;
    const uint8_t* src = (const uint8_t*)samples + startIndex;
    return ringWrite(src, count);
}

int soundPlaySilence(int count) {
    if (!gRunning || count <= 0) return 0;
    // Write zeros in chunks to avoid large stack alloc
    uint8_t zeros[1024];
    memset(zeros, 0, sizeof(zeros));
    int written = 0;
    while (written < count) {
        int chunk = count - written;
        if (chunk > (int)sizeof(zeros)) chunk = (int)sizeof(zeros);
        int got = ringWrite(zeros, chunk);
        written += got;
        if (got < chunk) break; // ring buffer full
    }
    return written;
}

float soundGetVolume(void) {
    if (gAudioQueue) {
        AudioQueueParameterValue vol = 0;
        if (AudioQueueGetParameter(gAudioQueue, kAudioQueueParam_Volume, &vol) == noErr) {
            return (float)vol;
        }
    }
    return gVolume;
}

void soundSetVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    gVolume = volume;
    if (gAudioQueue) {
        AudioQueueSetParameter(gAudioQueue, kAudioQueueParam_Volume, volume);
    }
}

bool soundIsRunning(void) {
    return gRunning;
}

#elif defined(_WIN32)
// =====================================================================
// Windows: waveOut (winmm). Same architecture as the Apple AudioQueue
// backend: VM thread -> lock-free ring buffer -> feeder -> speakers.
// waveOut API calls are NOT safe inside the waveOut callback (documented
// deadlock risk), so a CALLBACK_EVENT + dedicated feeder thread refills
// the WAVEHDR buffers instead.
// =====================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <cstring>

// ---- Ring buffer (lock-free SPSC), identical to the Apple one ----
static constexpr int kRingCapacity = 64 * 1024; // ~0.37s at 44100 stereo 16-bit
static uint8_t gRingBuffer[kRingCapacity];
static std::atomic<int> gRingHead{0}; // written by VM thread
static std::atomic<int> gRingTail{0}; // advanced by feeder thread

static int ringAvailableWrite() {
    int h = gRingHead.load(std::memory_order_relaxed);
    int t = gRingTail.load(std::memory_order_acquire);
    int used = (h - t + kRingCapacity) % kRingCapacity;
    return kRingCapacity - 1 - used;
}

static int ringAvailableRead() {
    int h = gRingHead.load(std::memory_order_acquire);
    int t = gRingTail.load(std::memory_order_relaxed);
    return (h - t + kRingCapacity) % kRingCapacity;
}

static int ringWrite(const uint8_t* data, int count) {
    int avail = ringAvailableWrite();
    if (count > avail) count = avail;
    if (count <= 0) return 0;
    int h = gRingHead.load(std::memory_order_relaxed);
    int firstChunk = kRingCapacity - h;
    if (firstChunk > count) firstChunk = count;
    memcpy(gRingBuffer + h, data, firstChunk);
    if (count > firstChunk) memcpy(gRingBuffer, data + firstChunk, count - firstChunk);
    gRingHead.store((h + count) % kRingCapacity, std::memory_order_release);
    return count;
}

static int ringRead(uint8_t* data, int count) {
    int avail = ringAvailableRead();
    if (count > avail) count = avail;
    if (count <= 0) return 0;
    int t = gRingTail.load(std::memory_order_relaxed);
    int firstChunk = kRingCapacity - t;
    if (firstChunk > count) firstChunk = count;
    memcpy(data, gRingBuffer + t, firstChunk);
    if (count > firstChunk) memcpy(data + firstChunk, gRingBuffer, count - firstChunk);
    gRingTail.store((t + count) % kRingCapacity, std::memory_order_release);
    return count;
}

// ---- waveOut output ----
static constexpr int kNumBuffers = 3;
static constexpr int kBufferByteSize = 4096; // ~23ms at 44100 stereo 16-bit

static HWAVEOUT gWaveOut = nullptr;
static WAVEHDR gHeaders[kNumBuffers];
static uint8_t gBufferData[kNumBuffers][kBufferByteSize];
static HANDLE gBufferEvent = nullptr;
static HANDLE gFeederThread = nullptr;
static std::atomic<bool> gQuit{false};
static bool gRunning = false;
static int gSemaIndex = 0;
static float gVolume = 1.0f;

typedef void (*SoundSemaSignalFunc)(int index);
static SoundSemaSignalFunc gSignalFunc = nullptr;
extern "C" void soundSetSignalFunc(SoundSemaSignalFunc fn) { gSignalFunc = fn; }

static DWORD WINAPI feederThreadProc(LPVOID) {
    while (!gQuit.load(std::memory_order_acquire)) {
        WaitForSingleObject(gBufferEvent, 100);
        if (gQuit.load(std::memory_order_acquire)) break;
        bool refilled = false;
        for (int i = 0; i < kNumBuffers; i++) {
            if (gHeaders[i].dwFlags & WHDR_DONE) {
                int got = ringRead(reinterpret_cast<uint8_t*>(gHeaders[i].lpData), kBufferByteSize);
                if (got < kBufferByteSize) {
                    memset(gHeaders[i].lpData + got, 0, kBufferByteSize - got);
                }
                gHeaders[i].dwBufferLength = kBufferByteSize;
                gHeaders[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(gWaveOut, &gHeaders[i], sizeof(WAVEHDR));
                refilled = true;
            }
        }
        // Signal VM semaphore that ring space opened up (mirrors the Apple
        // per-buffer-completion signal).
        if (refilled && gSemaIndex > 0 && gSignalFunc) {
            gSignalFunc(gSemaIndex);
        }
    }
    return 0;
}

bool soundInit(int sampleRate, bool stereo, int semaIndex) {
    if (gRunning) soundStop();

    gSemaIndex = semaIndex;
    gRingHead.store(0, std::memory_order_relaxed);
    gRingTail.store(0, std::memory_order_relaxed);
    gQuit.store(false, std::memory_order_relaxed);

    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = stereo ? 2 : 1;
    fmt.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * 2);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    gBufferEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!gBufferEvent) return false;

    if (waveOutOpen(&gWaveOut, WAVE_MAPPER, &fmt,
                    reinterpret_cast<DWORD_PTR>(gBufferEvent), 0,
                    CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(gBufferEvent);
        gBufferEvent = nullptr;
        gWaveOut = nullptr;
        return false;
    }

    // Prepare + prime all buffers with silence
    memset(gHeaders, 0, sizeof(gHeaders));
    for (int i = 0; i < kNumBuffers; i++) {
        memset(gBufferData[i], 0, kBufferByteSize);
        gHeaders[i].lpData = reinterpret_cast<LPSTR>(gBufferData[i]);
        gHeaders[i].dwBufferLength = kBufferByteSize;
        if (waveOutPrepareHeader(gWaveOut, &gHeaders[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
            waveOutWrite(gWaveOut, &gHeaders[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            soundStop();
            return false;
        }
    }

    gFeederThread = CreateThread(nullptr, 0, feederThreadProc, nullptr, 0, nullptr);
    if (!gFeederThread) {
        soundStop();
        return false;
    }

    gRunning = true;
    soundSetVolume(gVolume);
    return true;
}

void soundStop(void) {
    gQuit.store(true, std::memory_order_release);
    if (gBufferEvent) SetEvent(gBufferEvent);
    if (gFeederThread) {
        WaitForSingleObject(gFeederThread, 2000);
        CloseHandle(gFeederThread);
        gFeederThread = nullptr;
    }
    if (gWaveOut) {
        waveOutReset(gWaveOut);
        for (int i = 0; i < kNumBuffers; i++) {
            if (gHeaders[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(gWaveOut, &gHeaders[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(gWaveOut);
        gWaveOut = nullptr;
    }
    if (gBufferEvent) {
        CloseHandle(gBufferEvent);
        gBufferEvent = nullptr;
    }
    gRunning = false;
    gSemaIndex = 0;
}

int soundAvailableSpace(void) {
    if (!gRunning) return 0;
    return ringAvailableWrite();
}

int soundPlaySamples(const void* samples, int startIndex, int count) {
    if (!gRunning || count <= 0) return 0;
    const uint8_t* src = static_cast<const uint8_t*>(samples) + startIndex;
    return ringWrite(src, count);
}

int soundPlaySilence(int count) {
    if (!gRunning || count <= 0) return 0;
    uint8_t zeros[1024];
    memset(zeros, 0, sizeof(zeros));
    int written = 0;
    while (written < count) {
        int chunk = count - written;
        if (chunk > static_cast<int>(sizeof(zeros))) chunk = static_cast<int>(sizeof(zeros));
        int got = ringWrite(zeros, chunk);
        written += got;
        if (got < chunk) break;
    }
    return written;
}

float soundGetVolume(void) {
    return gVolume;
}

void soundSetVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    gVolume = volume;
    if (gWaveOut) {
        DWORD v = static_cast<DWORD>(volume * 0xFFFF);
        waveOutSetVolume(gWaveOut, (v << 16) | v);  // both channels
    }
}

bool soundIsRunning(void) {
    return gRunning;
}

#else
// Non-Apple, non-Windows stub implementations
bool soundInit(int, bool, int) { return false; }
void soundStop(void) {}
int soundAvailableSpace(void) { return 0; }
int soundPlaySamples(const void*, int, int) { return 0; }
int soundPlaySilence(int) { return 0; }
float soundGetVolume(void) { return 1.0f; }
void soundSetVolume(float) {}
bool soundIsRunning(void) { return false; }
extern "C" void soundSetSignalFunc(void (*)(int)) {}
#endif
