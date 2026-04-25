/*
 * ios.cpp - iOS device + simulator-specific implementations
 *
 * Code that's specific to iOS (sandbox stubs, no-Homebrew-paths,
 * UIKit-specific glue) lives here.  Apple-shared code (W^X,
 * CFRunLoop, CoreAudio glue) lives in apple_shared.cpp.
 *
 * Currently this file is empty — all platform functions VM core
 * needs for iOS are defined in apple_shared.cpp.  This file exists
 * so future iOS-only additions have a clear destination.
 */

#include "Platform.hpp"

namespace pharo {
namespace platform {

void platformInit() {
    // Future: iOS-specific sandbox checks, signal handlers.
}

}  // namespace platform
}  // namespace pharo
