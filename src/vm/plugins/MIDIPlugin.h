/*
 * MIDIPlugin.h - CoreMIDI wrapper for Pharo VM
 *
 * Convention: ports 0..N-1 are output (destinations),
 *             N..N+M-1 are input (sources).
 */

#ifndef PHARO_MIDI_PLUGIN_H
#define PHARO_MIDI_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize CoreMIDI client (lazy, called on first use).
bool midiInit(void);

// Shut down CoreMIDI client and reset state for VM relaunch.
void midiShutdown(void);

// Total port count (destinations + sources).
int midiGetPortCount(void);

// Get port name. Caller must free() the returned string.
char* midiGetPortName(int portIndex);

// Open a port. Returns a handle (>= 0) or -1 on error.
int midiOpenPort(int portIndex);

// Close a previously opened port.
void midiClosePort(int handle);

// Read available MIDI bytes from an input port into buf.
// Returns number of bytes read (0 if none available).
int midiRead(int handle, uint8_t* buf, int bufSize);

// Write raw MIDI bytes to an output port.
// Returns number of bytes written or -1 on error.
int midiWrite(int handle, const uint8_t* data, int count);

// Get MIDI clock in microseconds (monotonic).
int64_t midiGetClock(void);

// Send a 3-byte MIDI message (NoteOn, NoteOff, CC, PitchBend).
bool midiSendShort(int handle, uint8_t status, uint8_t data1, uint8_t data2);

// Send a 2-byte MIDI message (Program Change, Channel Pressure).
bool midiSendShort2(int handle, uint8_t status, uint8_t data1);

// Send SysEx (buffer must start with 0xF0 and end with 0xF7).
bool midiSendSysEx(int handle, const uint8_t* data, int count);

#ifdef __cplusplus
}
#endif

#endif
