/*
 * DisplaySurface.hpp - Abstract display interface for Pharo VM
 */

#ifndef PHARO_DISPLAY_SURFACE_HPP
#define PHARO_DISPLAY_SURFACE_HPP

#include <cstdint>
#include <cstddef>

namespace pharo {

/// Display surface that the VM renders to
class DisplaySurface {
public:
    virtual ~DisplaySurface() = default;

    /// Get surface dimensions
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual int depth() const = 0;

    /// Get pointer to pixel buffer (32-bit ARGB)
    virtual uint32_t* pixels() = 0;
    virtual size_t pitch() const = 0;  // bytes per row

    /// Notify that a region needs redraw
    virtual void invalidateRect(int x, int y, int w, int h) = 0;

    /// Force full redraw
    virtual void update() = 0;

    /// Apply a window-initiated resize, if one is pending (e.g. the user
    /// dragged the Win32 window frame). Called on the VM thread (from
    /// stub_SDL_PollEvent) so the buffer swap is ordered before the
    /// SIZE_CHANGED event the caller then pushes via ffi_notifyDisplayResize.
    /// Returns true and fills w/h when a resize was applied.
    virtual bool applyPendingResize(int& /*w*/, int& /*h*/) { return false; }
};

/// Global display surface (set by platform)
extern DisplaySurface* gDisplaySurface;

} // namespace pharo

#endif
