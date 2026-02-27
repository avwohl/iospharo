/*
 * SoundPlugin.h - Audio output via Audio Queue Services
 *
 * Provides a ring buffer fed by the VM thread and drained by the
 * audio system callback. Signals a Pharo semaphore when buffer
 * space becomes available.
 */

#ifndef PHARO_SOUND_PLUGIN_H
#define PHARO_SOUND_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize audio output. Returns true on success.
// sampleRate: e.g. 44100
// stereo: true for 2 channels, false for mono
// semaIndex: Pharo semaphore index to signal when buffer space frees up
bool soundInit(int sampleRate, bool stereo, int semaIndex);

// Stop and dispose audio output.
void soundStop(void);

// Returns available space in bytes in the ring buffer.
int soundAvailableSpace(void);

// Copy samples from VM buffer into the ring buffer.
// samples: pointer to 16-bit signed PCM samples
// startIndex: 0-based byte offset into samples
// count: number of bytes to copy
// Returns number of bytes actually written.
int soundPlaySamples(const void* samples, int startIndex, int count);

// Play silence (fills count bytes of zeros).
// Returns number of bytes written.
int soundPlaySilence(int count);

// Get current volume (0.0 to 1.0).
float soundGetVolume(void);

// Set output volume (0.0 to 1.0).
void soundSetVolume(float volume);

// Returns true if sound output is currently active.
bool soundIsRunning(void);

#ifdef __cplusplus
}
#endif

#endif
