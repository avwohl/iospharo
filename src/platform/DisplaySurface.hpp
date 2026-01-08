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
};

/// Global display surface (set by platform)
extern DisplaySurface* gDisplaySurface;

} // namespace pharo

#endif
