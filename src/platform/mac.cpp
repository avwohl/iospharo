/*
 * mac.cpp - macOS desktop + Catalyst-specific implementations
 *
 * Code that's specific to macOS (whether classic AppKit or Catalyst)
 * but NOT to iOS device builds lives here.  Apple-shared code (W^X,
 * CFRunLoop, CoreAudio glue) lives in apple_shared.cpp.
 *
 * Currently this file is empty — all platform functions VM core
 * needs for Mac are defined in apple_shared.cpp.  This file exists
 * so future Mac-only additions (Catalyst NSApp swizzles, app bundle
 * resolution) have a clear destination.
 */

#include "Platform.hpp"

namespace pharo {
namespace platform {

void platformInit() {
    // Future: install Catalyst NSApp/NSMenu swizzles, ObjC exception
    // handler, signal handlers.  For now the existing code in
    // PlatformBridge.cpp handles those (Phase 2 of the migration).
}

}  // namespace platform
}  // namespace pharo
