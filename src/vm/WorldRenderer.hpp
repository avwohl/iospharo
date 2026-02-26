/*
 * WorldRenderer.hpp - Direct rendering of World morphs to platform display
 *
 * Extracted from Interpreter.cpp. Renders a visual representation of the
 * Smalltalk World morph tree directly to the pixel buffer, bypassing
 * NullWorldRenderer. Used as a fallback when no Display Form is set.
 *
 * Includes: menu bar rendering, dropdown menus, world menu overlay.
 */

#ifndef PHARO_WORLD_RENDERER_HPP
#define PHARO_WORLD_RENDERER_HPP

#include "Oop.hpp"
#include "ObjectHeader.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace pharo {

class ObjectMemory;

class WorldRenderer {
public:
    explicit WorldRenderer(ObjectMemory& memory);

    /// Render the World's morph tree to the display surface.
    void render();

    /// Visit all Oop roots owned by WorldRenderer (for GC).
    template<typename Visitor>
    void forEachOopRoot(Visitor&& visitor);

    // ===== Accessors for Interpreter event handling =====

    int menuBarTop() const { return menuBarTop_; }
    int menuBarBottom() const { return menuBarBottom_; }
    int selectedMenuIndex() const { return selectedMenuIndex_; }
    void setSelectedMenuIndex(int idx) { selectedMenuIndex_ = idx; }

    const std::vector<std::pair<int, int>>& menuItemBounds() const { return menuItemBounds_; }
    const std::vector<Oop>& menuBarItemMorphs() const { return menuBarItemMorphs_; }

    struct DropdownState {
        int x = 0, y = 0, width = 0, height = 0;
        int lineHeight = 0;
        std::vector<Oop> itemMorphs;
        bool valid = false;
        int64_t openTimeMs = 0;
    };

    DropdownState& dropdownState() { return dropdownState_; }
    const DropdownState& dropdownState() const { return dropdownState_; }

    struct WorldMenuBounds {
        int x = 0, y = 0, width = 0, height = 0;
    };

    WorldMenuBounds& pendingMenuBounds() { return pendingMenuBounds_; }
    bool hasVisibleMenu() const { return hasVisibleMenu_; }
    void setHasVisibleMenu(bool v) { hasVisibleMenu_ = v; }

    struct DirtyRect {
        int x1, y1, x2, y2;
        bool valid = false;
    };

    DirtyRect& dirtyMenuDropdown() { return dirtyMenuDropdown_; }

private:
    ObjectMemory& memory_;

    // Menu interaction state
    int selectedMenuIndex_ = -1;
    int prevSelectedMenuIndex_ = -1;
    int64_t lastMenuClickTime_ = 0;
    int lastMenuClickX_ = -1000;
    int lastMenuClickY_ = -1000;
    std::vector<std::pair<int, int>> menuItemBounds_;
    std::vector<Oop> menuBarItemMorphs_;
    int menuBarTop_ = 28;
    int menuBarBottom_ = 72;
    DropdownState dropdownState_;
    WorldMenuBounds pendingMenuBounds_;
    bool hasVisibleMenu_ = false;
    DirtyRect dirtyMenuDropdown_;

    // ===== Private helpers =====

    std::string getMorphClassName(Oop morph) const;
    uint32_t extractColor(Oop colorObj) const;
    bool extractRect(Oop rect, int& x1, int& y1, int& x2, int& y2) const;
    bool extractBounds(Oop morph, int dispWidth, int dispHeight,
                       int& x1, int& y1, int& x2, int& y2) const;
    std::string extractString(Oop strObj) const;
    void renderMenuBar(uint32_t* pixels, int dispWidth, int dispHeight,
                       Oop menubarMorph, int menuBarHeight);
    void renderMorph(uint32_t* pixels, int dispWidth, int dispHeight,
                     Oop morph, int depth, int index, int& totalMorphsDrawn);
    void renderWorldMenuOverlay(uint32_t* pixels, int dispWidth, int dispHeight);
};

// ===== TEMPLATE IMPLEMENTATIONS =====

template<typename Visitor>
void WorldRenderer::forEachOopRoot(Visitor&& visitor) {
    for (auto& morph : menuBarItemMorphs_) {
        visitor(morph);
    }
    for (auto& morph : dropdownState_.itemMorphs) {
        visitor(morph);
    }
}

} // namespace pharo

#endif // PHARO_WORLD_RENDERER_HPP
