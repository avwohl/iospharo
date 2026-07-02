/*
 * windows.cpp - Windows platform implementations
 *
 * Windows joins macOS and Linux as a tier-1 host.  It is closely
 * analogous to Linux (src/platform/linux.cpp):
 *
 *   - Allocate JIT code memory as RWX once (VirtualAlloc with
 *     PAGE_EXECUTE_READWRITE) and never re-protect.  Windows, unlike
 *     iOS, permits RWX pages, so makeWritable / makeExecutable are
 *     no-ops — the same per-call W^X win we get on Linux.
 *
 *   - Replace the macOS CFRunLoop pump with Sleep()/SwitchToThread().
 *     Headless / SUnit builds have no UI loop to service.  Future GUI
 *     work (milestone 4) may need to pump a Win32 message loop here.
 *
 * Built with clang (LLVM-MinGW); MSVC cl.exe cannot compile the VM
 * (Interpreter.cpp uses computed gotos + GNU inline asm).  See
 * docs/windows-port-plan.md.
 *
 * Linked into Windows builds only.  Never linked on Apple or Linux.
 */

// GetCurrentThreadStackLimits needs Windows 8 (0x0602).  CMake also
// passes this; the guard keeps the file self-contained.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// winsock2.h must precede windows.h so the modern Winsock API wins
// over the legacy winsock.h that windows.h would otherwise pull in.
#include <winsock2.h>
#include <windows.h>

#include "Platform.hpp"
#include "DisplaySurface.hpp"

#include <string>

// =====================================================================
// Windows implementations of symbols normally provided by the iOS /
// Catalyst host app (PlatformBridge.cpp).  Clipboard is the real Win32
// clipboard (UTF-16 <-> UTF-8, like real SDL2's SDL_windowsclipboard.c);
// IME/text-input remains a no-op.  Mirrors the Linux block in
// linux.cpp:32-39.
// =====================================================================

namespace pharo {
DisplaySurface* gDisplaySurface = nullptr;
}

// Another process holding the clipboard makes OpenClipboard fail
// transiently; retry briefly (real SDL2 does the same).
static bool openClipboardWithRetry() {
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(5);
    }
    return false;
}

// Returns UTF-8 text; pointer is valid until the next call (the FFI stub
// strdups it immediately — stub_SDL_GetClipboardText in FFI.cpp).
extern "C" const char* vm_getClipboardText(void) {
    static std::string sClipboardUtf8;
    sClipboardUtf8.clear();
    if (!openClipboardWithRetry()) return "";
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                sClipboardUtf8.resize(static_cast<size_t>(len) - 1);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, &sClipboardUtf8[0], len, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return sClipboardUtf8.c_str();
}

extern "C" void vm_setClipboardText(const char* text) {
    if (!text) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wlen <= 0) return;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen) * sizeof(wchar_t));
    if (!hMem) return;
    if (wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem))) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, dst, wlen);
        GlobalUnlock(hMem);
    } else {
        GlobalFree(hMem);
        return;
    }
    if (!openClipboardWithRetry()) {
        GlobalFree(hMem);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);  // ownership transfers to the system only on success
    }
    CloseClipboard();
}

extern "C" void vm_startTextInput(void) {}
extern "C" void vm_stopTextInput(void) {}

namespace pharo {
namespace platform {

// ===== JIT code zone (RWX from VirtualAlloc, no per-call flips) =====

void* allocCodeMemory(size_t bytes) {
    // VirtualAlloc returns NULL on failure — exactly the contract
    // allocCodeMemory promises.
    return VirtualAlloc(nullptr, bytes,
                        MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
}

void freeCodeMemory(void* ptr, size_t /*bytes*/) {
    // MEM_RELEASE requires the size argument to be 0.
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

int currentWXShadowMode() { return 1; }  // RWX platform: never W-only

bool makeWritable(void* /*ptr*/, size_t /*bytes*/) {
    // No-op: pages are already RWX from VirtualAlloc.  See linux.cpp.
    return true;
}

bool makeExecutable(void* /*ptr*/, size_t /*bytes*/) {
    // No-op: see makeWritable.
    return true;
}

void flushICache(void* ptr, size_t bytes) {
    // x86_64 has coherent I/D caches so this is effectively a no-op,
    // but FlushInstructionCache is the documented, future-proof call
    // (and correct should a Windows-on-ARM64 build ever land here).
    FlushInstructionCache(GetCurrentProcess(), ptr, bytes);
}

// flipJitToWritable / flipJitToExecutable are defined inline in
// Platform.hpp's #else branch (no-ops on non-Apple) — do NOT redefine.

// ===== Stack bounds =====

bool getStackBounds(uint8_t** top, uint8_t** bot) {
    // GetCurrentThreadStackLimits reports [low, high) directly; *top is
    // the HIGH end (frames grow downward from it), *bot the LOW end.
    *top = nullptr;
    *bot = nullptr;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    if (!low || !high) return false;
    *bot = reinterpret_cast<uint8_t*>(low);
    *top = reinterpret_cast<uint8_t*>(high);
    return true;
}

// jitTrampolineJMSize is defined in src/platform/jit_jmsize_{arm64,other}.cpp
// (per-arch file selected by CMake) — do NOT redefine it here.

// ===== Cooperative scheduling =====

void relinquishCPU(uint64_t microseconds) {
    // No host UI loop to pump on headless Windows builds.  Sub-ms
    // yields can't be expressed by Sleep's ms granularity, so hand the
    // rest of the timeslice to another ready thread instead.
    if (microseconds == 0) return;
    if (microseconds < 1000) {
        SwitchToThread();
    } else {
        Sleep(static_cast<DWORD>(microseconds / 1000));
    }
}

// ===== One-time init =====

void platformInit() {
    // Winsock must be initialized before any getaddrinfo / socket call.
    // The core DNS path (Primitives.cpp) needs this even when the full
    // SocketPlugin is excluded from the milestone-1 build.
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

}  // namespace platform
}  // namespace pharo
