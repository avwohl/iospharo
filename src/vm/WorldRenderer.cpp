/*
 * WorldRenderer.cpp - Direct rendering of World morphs to platform display
 *
 * Extracted from Interpreter.cpp. Renders a visual representation of the
 * Smalltalk World morph tree directly to the pixel buffer.
 */

#include "WorldRenderer.hpp"
#include "ObjectMemory.hpp"
#include "../platform/DisplaySurface.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

#if __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <CoreFoundation/CFRunLoop.h>
#undef nil
#endif

namespace pharo {

// ===== drawText (static helper) =====

static void drawText(uint32_t* pixels, int dispWidth, int dispHeight,
                     int x, int y, const std::string& text, uint32_t color, int fontSize = 14) {
#if __APPLE__
    if (text.empty()) return;

    CGFloat alpha = ((color >> 24) & 0xFF) / 255.0;
    CGFloat red = ((color >> 16) & 0xFF) / 255.0;
    CGFloat green = ((color >> 8) & 0xFF) / 255.0;
    CGFloat blue = (color & 0xFF) / 255.0;

    int textWidth = static_cast<int>(text.length()) * fontSize;
    int textHeight = static_cast<int>(fontSize * 2.0);

    if (x < 0 || y < 0 || x >= dispWidth || y >= dispHeight) return;
    if (x + textWidth > dispWidth) textWidth = dispWidth - x;
    if (y + textHeight > dispHeight) textHeight = dispHeight - y;

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        nullptr, textWidth, textHeight, 8, textWidth * 4, colorSpace,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host
    );
    CGColorSpaceRelease(colorSpace);

    if (!ctx) return;

    CGContextClearRect(ctx, CGRectMake(0, 0, textWidth, textHeight));
    CGContextTranslateCTM(ctx, 0, textHeight);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextSetRGBFillColor(ctx, red, green, blue, alpha);
    CGContextSetTextDrawingMode(ctx, kCGTextFill);

    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), fontSize, nullptr);
    if (!font) {
        font = CTFontCreateWithName(CFSTR("Arial"), fontSize, nullptr);
    }
    if (!font) {
        CGContextRelease(ctx);
        return;
    }

    CFStringRef cfText = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    if (!cfText) {
        CFRelease(font);
        CGContextRelease(ctx);
        return;
    }

    CFStringRef keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    CFTypeRef values[] = { font, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, (const void**)keys, (const void**)values, 2,
                                                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFAttributedStringRef attrString = CFAttributedStringCreate(nullptr, cfText, attrs);
    CFRelease(cfText);
    CFRelease(attrs);

    if (!attrString) {
        CFRelease(font);
        CGContextRelease(ctx);
        return;
    }

    CTLineRef line = CTLineCreateWithAttributedString(attrString);
    CFRelease(attrString);

    if (line) {
        CGFloat baseline = fontSize * 0.95;
        CGContextSetTextPosition(ctx, 0, baseline);
        CTLineDraw(line, ctx);
        CFRelease(line);
    }

    CFRelease(font);

    uint32_t* textPixels = (uint32_t*)CGBitmapContextGetData(ctx);
    if (textPixels) {
        for (int ty = 0; ty < textHeight; ty++) {
            for (int tx = 0; tx < textWidth; tx++) {
                int destX = x + tx;
                int destY = y + ty;
                if (destX >= 0 && destX < dispWidth && destY >= 0 && destY < dispHeight) {
                    int srcY = textHeight - 1 - ty;
                    uint32_t srcPixel = textPixels[srcY * textWidth + tx];
                    uint8_t srcAlpha = (srcPixel >> 24) & 0xFF;
                    uint8_t srcR = (srcPixel >> 16) & 0xFF;
                    uint8_t srcG = (srcPixel >> 8) & 0xFF;
                    uint8_t srcB = srcPixel & 0xFF;
                    if (srcAlpha > 0) {
                        uint32_t destPixel = pixels[destY * dispWidth + destX];
                        uint8_t destR = (destPixel >> 16) & 0xFF;
                        uint8_t destG = (destPixel >> 8) & 0xFF;
                        uint8_t destB = destPixel & 0xFF;

                        uint8_t outR = srcR + (destR * (255 - srcAlpha)) / 255;
                        uint8_t outG = srcG + (destG * (255 - srcAlpha)) / 255;
                        uint8_t outB = srcB + (destB * (255 - srcAlpha)) / 255;

                        pixels[destY * dispWidth + destX] = 0xFF000000 | (outR << 16) | (outG << 8) | outB;
                    }
                }
            }
        }
    }

    CGContextRelease(ctx);
#endif
}

// ===== WorldRenderer implementation =====

WorldRenderer::WorldRenderer(ObjectMemory& memory)
    : memory_(memory) {}

std::string WorldRenderer::getMorphClassName(Oop morph) const {
    std::string name = memory_.classNameOf(morph);
    return name == "?" ? "Unknown" : name;
}

uint32_t WorldRenderer::extractColor(Oop colorObj) const {
    Oop nilObj = memory_.nil();
    if (colorObj.isNil() || !colorObj.isObject()) return 0xFFCCCCCC;

    Oop rgb = memory_.fetchPointer(0, colorObj);

    if (rgb.isSmallInteger()) {
        int rgbVal = static_cast<int>(rgb.asSmallInteger());
        return 0xFF000000 | (rgbVal & 0xFFFFFF);
    }

    if (rgb.isObject()) {
        ObjectHeader* rgbHdr = rgb.asObjectPtr();
        if (rgbHdr->isBytesObject() && rgbHdr->byteSize() == 8) {
            double* floatVal = reinterpret_cast<double*>(rgbHdr->bytes());
            int rgbInt = static_cast<int>(*floatVal) & 0xFFFFFF;
            return 0xFF000000 | rgbInt;
        }
    }

    return 0xFFCCCCCC;
}

bool WorldRenderer::extractRect(Oop rect, int& x1, int& y1, int& x2, int& y2) const {
    if (rect.isNil() || !rect.isObject()) return false;

    Oop origin = memory_.fetchPointer(0, rect);
    Oop corner = memory_.fetchPointer(1, rect);
    if (origin.isNil() || corner.isNil()) return false;
    if (!origin.isObject() || !corner.isObject()) return false;

    Oop originX = memory_.fetchPointer(0, origin);
    Oop originY = memory_.fetchPointer(1, origin);
    Oop cornerX = memory_.fetchPointer(0, corner);
    Oop cornerY = memory_.fetchPointer(1, corner);

    auto extractCoord = [](Oop coord) -> int {
        if (coord.isSmallInteger()) {
            return static_cast<int>(coord.asSmallInteger());
        } else if (coord.isSmallFloat()) {
            return static_cast<int>(coord.asSmallFloat());
        }
        return 0;
    };

    x1 = extractCoord(originX);
    y1 = extractCoord(originY);
    x2 = extractCoord(cornerX);
    y2 = extractCoord(cornerY);

    return true;
}

bool WorldRenderer::extractBounds(Oop morph, int dispWidth, int dispHeight,
                                   int& x1, int& y1, int& x2, int& y2) const {
    // Try bounds (slot 0) first
    if (extractRect(memory_.fetchPointer(0, morph), x1, y1, x2, y2)) {
        if (x2 - x1 > 0 && y2 - y1 > 0) return true;
    }

    // Try fullBounds (slot 3) as fallback
    if (extractRect(memory_.fetchPointer(3, morph), x1, y1, x2, y2)) {
        if (x2 - x1 > 0 && y2 - y1 > 0) return true;
    }

    // Assign fallback bounds based on morph class
    std::string className = getMorphClassName(morph);
    if (className == "MenubarMorph") {
        x1 = 0; y1 = 0; x2 = dispWidth; y2 = 25;
        return true;
    } else if (className == "TaskbarMorph") {
        x1 = 0; y1 = dispHeight - 40; x2 = dispWidth; y2 = dispHeight;
        return true;
    } else if (className == "SpWindow" || className == "SystemWindow") {
        x1 = 50; y1 = 50; x2 = dispWidth - 100; y2 = dispHeight - 100;
        return true;
    } else if (className.find("Grip") != std::string::npos) {
        return false;
    }

    return false;
}

std::string WorldRenderer::extractString(Oop strObj) const {
    return memory_.oopToString(strObj);
}

void WorldRenderer::renderMenuBar(uint32_t* pixels, int dispWidth, int dispHeight,
                                   Oop menubarMorph, int menuBarHeight) {
    int titleBarOffset = (dispHeight > 1000) ? 56 : 28;
    int scaledMenuBarHeight = (dispHeight > 1000) ? menuBarHeight * 2 : menuBarHeight;

    // Draw title area background
    uint32_t titleAreaColor = 0xFFF6F6F6;
    for (int y = 0; y < titleBarOffset && y < dispHeight; y++) {
        for (int x = 0; x < dispWidth; x++) {
            pixels[y * dispWidth + x] = titleAreaColor;
        }
    }
    // Menu bar with subtle gradient
    for (int y = titleBarOffset; y < titleBarOffset + scaledMenuBarHeight; y++) {
        int progress = y - titleBarOffset;
        int gray = 246 - (progress * 14 / scaledMenuBarHeight);
        uint32_t menuBarColor = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
        for (int x = 0; x < dispWidth; x++) {
            if (y < dispHeight) {
                pixels[y * dispWidth + x] = menuBarColor;
            }
        }
    }

    // Bottom border
    int borderY = titleBarOffset + scaledMenuBarHeight - 1;
    if (borderY < dispHeight) {
        for (int x = 0; x < dispWidth; x++) {
            pixels[borderY * dispWidth + x] = 0xFFD0D0D0;
        }
    }

    // Get submorphs (menu items)
    Oop submorphs = memory_.fetchPointer(2, menubarMorph);
    if (submorphs.isNil() || !submorphs.isObject()) {
        return;
    }

    ObjectHeader* subHdr = submorphs.asObjectPtr();
    size_t numItems = subHdr->slotCount();
    uint32_t textColor = 0xFF1A1A1A;

    // Collect menu item labels and store morphs for dropdown access
    std::vector<std::string> labels;
    menuBarItemMorphs_.clear();
    for (size_t i = 0; i < numItems; i++) {
        Oop item = subHdr->slotAt(i);
        if (item.isNil() || !item.isObject()) continue;

        std::string label;

        ObjectHeader* itemHdr = item.asObjectPtr();
        for (size_t slot = 5; slot < std::min((size_t)15, itemHdr->slotCount()); slot++) {
            Oop slotVal = memory_.fetchPointer(slot, item);
            if (slotVal.isNil() || !slotVal.isObject()) continue;

            ObjectHeader* slotHdr = slotVal.asObjectPtr();
            if (slotHdr->isBytesObject() && slotHdr->byteSize() > 0 && slotHdr->byteSize() < 50) {
                std::string s((char*)slotHdr->bytes(), slotHdr->byteSize());
                bool valid = true;
                for (char c : s) {
                    if (c < 32 || c > 126) { valid = false; break; }
                }
                if (valid && s.length() > 0 && s.length() < 20) {
                    label = s;
                    break;
                }
            }
        }

        // If no label found, check submorphs for StringMorph
        if (label.empty()) {
            Oop itemSubmorphs = memory_.fetchPointer(2, item);
            if (!itemSubmorphs.isNil() && itemSubmorphs.isObject()) {
                ObjectHeader* isHdr = itemSubmorphs.asObjectPtr();
                for (size_t j = 0; j < isHdr->slotCount(); j++) {
                    Oop subm = isHdr->slotAt(j);
                    if (subm.isNil() || !subm.isObject()) continue;

                    ObjectHeader* submHdr = subm.asObjectPtr();
                    for (size_t slot = 5; slot < std::min((size_t)15, submHdr->slotCount()); slot++) {
                        Oop slotVal = memory_.fetchPointer(slot, subm);
                        if (slotVal.isNil() || !slotVal.isObject()) continue;

                        ObjectHeader* svHdr = slotVal.asObjectPtr();
                        if (svHdr->isBytesObject() && svHdr->byteSize() > 0 && svHdr->byteSize() < 50) {
                            std::string s((char*)svHdr->bytes(), svHdr->byteSize());
                            bool valid = true;
                            for (char c : s) {
                                if (c < 32 || c > 126) { valid = false; break; }
                            }
                            if (valid && s.length() > 0 && s.length() < 20) {
                                label = s;
                                break;
                            }
                        }
                    }
                    if (!label.empty()) break;
                }
            }
        }

        if (!label.empty()) {
            labels.push_back(label);
            menuBarItemMorphs_.push_back(item);
        }
    }

    // Draw menu item labels with proper spacing
    bool isRetina = dispWidth > 1500;
    int fontSize = isRetina ? 48 : 24;
    int textX = isRetina ? 32 : 16;
    int menuBarCenter = titleBarOffset + scaledMenuBarHeight / 2;
    int xHeightCenterInBuffer = static_cast<int>(fontSize * 0.70);
    int textY = menuBarCenter - xHeightCenterInBuffer;
    int itemSpacing = isRetina ? 56 : 28;
    int charWidth = isRetina ? 26 : 13;

    menuItemBounds_.clear();

    int itemIndex = 0;
    for (const std::string& label : labels) {
        int itemWidth = static_cast<int>(label.length()) * charWidth;
        int itemPadding = isRetina ? 16 : 8;

        // Highlight selected menu item
        if (selectedMenuIndex_ == itemIndex) {
            uint32_t highlightColor = 0xFF3478F6;
            for (int hy = titleBarOffset; hy < titleBarOffset + scaledMenuBarHeight - 1; hy++) {
                for (int hx = textX - itemPadding; hx < textX + itemWidth + itemPadding && hx < dispWidth; hx++) {
                    if (hx >= 0 && hy >= 0 && hy < dispHeight) {
                        pixels[hy * dispWidth + hx] = highlightColor;
                    }
                }
            }
            drawText(pixels, dispWidth, dispHeight, textX, textY, label, 0xFFFFFFFF, fontSize);
        } else {
            drawText(pixels, dispWidth, dispHeight, textX, textY, label, textColor, fontSize);
        }

        menuItemBounds_.push_back({textX - itemPadding, textX + itemWidth + itemPadding});
        textX += itemWidth + itemSpacing;
        itemIndex++;
    }

    // Store menu bar bounds for click detection
    menuBarTop_ = titleBarOffset;
    menuBarBottom_ = titleBarOffset + scaledMenuBarHeight;

    // Render dropdown menu if a menu is selected
    if (selectedMenuIndex_ >= 0 && selectedMenuIndex_ < static_cast<int>(menuBarItemMorphs_.size())) {
        Oop selectedItem = menuBarItemMorphs_[selectedMenuIndex_];

        // Find the menu associated with this menu bar item
        Oop menuMorph = Oop::nil();
        ObjectHeader* selItemHdr = selectedItem.asObjectPtr();

        for (size_t slot = 5; slot < std::min((size_t)20, selItemHdr->slotCount()); slot++) {
            Oop slotVal = memory_.fetchPointer(slot, selectedItem);
            if (slotVal.isNil() || !slotVal.isObject()) continue;

            std::string cn = memory_.classNameOf(slotVal);
            if (cn.find("Menu") != std::string::npos) {
                menuMorph = slotVal;
                break;
            }
        }

        // Collect dropdown item labels
        std::vector<std::string> dropdownLabels;
        std::vector<Oop> dropdownItemMorphs;

        if (!menuMorph.isNil() && menuMorph.isObject()) {
            Oop menuSubmorphs = memory_.fetchPointer(2, menuMorph);
            if (!menuSubmorphs.isNil() && menuSubmorphs.isObject()) {
                ObjectHeader* msHdr = menuSubmorphs.asObjectPtr();
                size_t numMenuItems = msHdr->slotCount();

                for (size_t mi = 0; mi < numMenuItems; mi++) {
                    Oop menuItem = msHdr->slotAt(mi);
                    if (menuItem.isNil() || !menuItem.isObject()) continue;

                    std::string itemLabel;
                    ObjectHeader* miHdr = menuItem.asObjectPtr();

                    for (size_t slot = 5; slot < std::min((size_t)15, miHdr->slotCount()); slot++) {
                        Oop slotVal = memory_.fetchPointer(slot, menuItem);
                        if (slotVal.isNil() || !slotVal.isObject()) continue;

                        ObjectHeader* slotHdr = slotVal.asObjectPtr();
                        if (slotHdr->isBytesObject() && slotHdr->byteSize() > 0 && slotHdr->byteSize() < 100) {
                            std::string s((char*)slotHdr->bytes(), slotHdr->byteSize());
                            bool valid = true;
                            for (char c : s) {
                                if (c < 32 || c > 126) { valid = false; break; }
                            }
                            if (valid && s.length() > 0) {
                                itemLabel = s;
                                break;
                            }
                        }
                    }

                    if (itemLabel.empty()) {
                        Oop itemSubmorphs = memory_.fetchPointer(2, menuItem);
                        if (!itemSubmorphs.isNil() && itemSubmorphs.isObject()) {
                            ObjectHeader* isHdr = itemSubmorphs.asObjectPtr();
                            for (size_t j = 0; j < isHdr->slotCount() && itemLabel.empty(); j++) {
                                Oop subm = isHdr->slotAt(j);
                                if (subm.isNil() || !subm.isObject()) continue;

                                ObjectHeader* submHdr = subm.asObjectPtr();
                                for (size_t slot = 5; slot < std::min((size_t)15, submHdr->slotCount()); slot++) {
                                    Oop slotVal = memory_.fetchPointer(slot, subm);
                                    if (slotVal.isNil() || !slotVal.isObject()) continue;

                                    ObjectHeader* svHdr = slotVal.asObjectPtr();
                                    if (svHdr->isBytesObject() && svHdr->byteSize() > 0 && svHdr->byteSize() < 100) {
                                        std::string s((char*)svHdr->bytes(), svHdr->byteSize());
                                        bool valid = true;
                                        for (char c : s) {
                                            if (c < 32 || c > 126) { valid = false; break; }
                                        }
                                        if (valid && s.length() > 0) {
                                            itemLabel = s;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (!itemLabel.empty()) {
                        dropdownLabels.push_back(itemLabel);
                        dropdownItemMorphs.push_back(menuItem);
                    }
                }
            }
        }

        // Draw dropdown if we have items
        if (!dropdownLabels.empty()) {
            int dropdownFontSize = fontSize;
            int lineHeight = static_cast<int>(dropdownFontSize * 1.5);
            int dropdownPadding = isRetina ? 16 : 8;

            int maxLabelWidth = 0;
            for (const auto& lbl : dropdownLabels) {
                int w = static_cast<int>(lbl.length()) * charWidth;
                if (w > maxLabelWidth) maxLabelWidth = w;
            }

            int dropdownWidth = maxLabelWidth + dropdownPadding * 4;
            int dropdownHeight = static_cast<int>(dropdownLabels.size()) * lineHeight + dropdownPadding * 2;

            int dropdownX = (selectedMenuIndex_ < static_cast<int>(menuItemBounds_.size()))
                ? menuItemBounds_[selectedMenuIndex_].first : textX;
            int dropdownY = titleBarOffset + scaledMenuBarHeight;

            uint32_t bgColor = 0xFFFAFAFA;
            uint32_t borderClr = 0xFFCCCCCC;
            uint32_t shadowColor = 0x20000000;

            // Shadow
            for (int sy = dropdownY + 2; sy < dropdownY + dropdownHeight + 4 && sy < dispHeight; sy++) {
                for (int sx = dropdownX + 2; sx < dropdownX + dropdownWidth + 4 && sx < dispWidth; sx++) {
                    if (sx >= 0 && sy >= 0) {
                        pixels[sy * dispWidth + sx] = shadowColor;
                    }
                }
            }

            // Background
            for (int dy = dropdownY; dy < dropdownY + dropdownHeight && dy < dispHeight; dy++) {
                for (int dx = dropdownX; dx < dropdownX + dropdownWidth && dx < dispWidth; dx++) {
                    if (dx >= 0 && dy >= 0) {
                        pixels[dy * dispWidth + dx] = bgColor;
                    }
                }
            }

            // Border
            for (int dx = dropdownX; dx < dropdownX + dropdownWidth && dx < dispWidth; dx++) {
                if (dx >= 0) {
                    if (dropdownY >= 0 && dropdownY < dispHeight)
                        pixels[dropdownY * dispWidth + dx] = borderClr;
                    int bottomY = dropdownY + dropdownHeight - 1;
                    if (bottomY >= 0 && bottomY < dispHeight)
                        pixels[bottomY * dispWidth + dx] = borderClr;
                }
            }
            for (int dy = dropdownY; dy < dropdownY + dropdownHeight && dy < dispHeight; dy++) {
                if (dy >= 0) {
                    if (dropdownX >= 0 && dropdownX < dispWidth)
                        pixels[dy * dispWidth + dropdownX] = borderClr;
                    int rightX = dropdownX + dropdownWidth - 1;
                    if (rightX >= 0 && rightX < dispWidth)
                        pixels[dy * dispWidth + rightX] = borderClr;
                }
            }

            // Menu item labels
            int itemY = dropdownY + dropdownPadding;
            for (size_t di = 0; di < dropdownLabels.size(); di++) {
                drawText(pixels, dispWidth, dispHeight,
                         dropdownX + dropdownPadding * 2, itemY,
                         dropdownLabels[di], textColor, dropdownFontSize);
                itemY += lineHeight;
            }

            // Store dropdown state for click handling
            dropdownState_.x = dropdownX;
            dropdownState_.y = dropdownY;
            dropdownState_.width = dropdownWidth;
            dropdownState_.height = dropdownHeight;
            dropdownState_.lineHeight = lineHeight;
            dropdownState_.itemMorphs = dropdownItemMorphs;
            dropdownState_.valid = true;
            dropdownState_.openTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        } else {
            dropdownState_.valid = false;
        }
    } else {
        dropdownState_.valid = false;
    }
}

void WorldRenderer::renderMorph(uint32_t* pixels, int dispWidth, int dispHeight,
                                 Oop morph, int depth, int index, int& totalMorphsDrawn) {
    if (morph.isNil() || !morph.isObject()) return;
    if (depth > 20) return;

    std::string className = getMorphClassName(morph);

    if (className == "MenubarMorph") {
        renderMenuBar(pixels, dispWidth, dispHeight, morph, 44);
        return;
    }

    if (className == "TaskbarMorph") {
        int taskbarHeight = 40;
        int taskbarY = dispHeight - taskbarHeight;
        uint32_t taskbarColor = 0xFF2A2A2A;
        for (int y = taskbarY; y < dispHeight; y++) {
            for (int x = 0; x < dispWidth; x++) {
                pixels[y * dispWidth + x] = taskbarColor;
            }
        }
        totalMorphsDrawn++;
        return;
    }

    totalMorphsDrawn++;

    int x1, y1, x2, y2;
    if (!extractBounds(morph, dispWidth, dispHeight, x1, y1, x2, y2)) return;

    Oop morphColor = memory_.fetchPointer(4, morph);
    uint32_t colorARGB = extractColor(morphColor);

    if ((colorARGB & 0xFFFFFF) == 0) return;

    bool hasBounds = (x2 - x1 >= 1 && y2 - y1 >= 1);

    int cx1 = std::max(0, std::min(x1, dispWidth));
    int cy1 = std::max(0, std::min(y1, dispHeight));
    int cx2 = std::max(0, std::min(x2, dispWidth));
    int cy2 = std::max(0, std::min(y2, dispHeight));

    if (hasBounds && cx2 > cx1 && cy2 > cy1) {
        for (int y = cy1; y < cy2; y++) {
            for (int x = cx1; x < cx2; x++) {
                pixels[y * dispWidth + x] = colorARGB;
            }
        }

        uint32_t borderColor = ((colorARGB & 0xFF000000)) |
                               (((colorARGB >> 16) & 0xFF) * 3 / 4 << 16) |
                               (((colorARGB >> 8) & 0xFF) * 3 / 4 << 8) |
                               ((colorARGB & 0xFF) * 3 / 4);

        for (int x = cx1; x < cx2; x++) {
            if (cy1 < dispHeight) pixels[cy1 * dispWidth + x] = borderColor;
            if (cy2 - 1 >= 0 && cy2 - 1 < dispHeight) pixels[(cy2 - 1) * dispWidth + x] = borderColor;
        }
        for (int y = cy1; y < cy2; y++) {
            if (cx1 < dispWidth) pixels[y * dispWidth + cx1] = borderColor;
            if (cx2 - 1 >= 0 && cx2 - 1 < dispWidth) pixels[y * dispWidth + cx2 - 1] = borderColor;
        }
    }

    Oop submorphs = memory_.fetchPointer(2, morph);
    if (submorphs.isNil() || !submorphs.isObject()) return;

    ObjectHeader* subHdr = submorphs.asObjectPtr();
    size_t numSubmorphs = subHdr->slotCount();

    for (size_t i = 0; i < numSubmorphs; i++) {
        Oop submorph = subHdr->slotAt(i);
        renderMorph(pixels, dispWidth, dispHeight, submorph, depth + 1, static_cast<int>(i), totalMorphsDrawn);
    }
}

void WorldRenderer::renderWorldMenuOverlay(uint32_t* pixels, int dispWidth, int dispHeight) {
    int mx = pendingMenuBounds_.x;
    int my = pendingMenuBounds_.y;
    int mw = pendingMenuBounds_.width;
    int mh = pendingMenuBounds_.height;
    int pitchPixels = dispWidth;  // pixels buffer is dispWidth wide

    // White background
    for (int dy = 0; dy < mh && my + dy < dispHeight; dy++) {
        for (int dx = 0; dx < mw && mx + dx < dispWidth; dx++) {
            int px = mx + dx;
            int py = my + dy;
            if (px >= 0 && py >= 0) {
                pixels[py * pitchPixels + px] = 0xFFFFFFFF;
            }
        }
    }

    // Gray border (2 pixels)
    uint32_t borderColor = 0xFF808080;
    for (int dx = 0; dx < mw; dx++) {
        for (int t = 0; t < 2; t++) {
            int px = mx + dx;
            int py1 = my + t;
            int py2 = my + mh - 1 - t;
            if (px >= 0 && px < dispWidth) {
                if (py1 >= 0 && py1 < dispHeight) pixels[py1 * pitchPixels + px] = borderColor;
                if (py2 >= 0 && py2 < dispHeight) pixels[py2 * pitchPixels + px] = borderColor;
            }
        }
    }
    for (int dy = 0; dy < mh; dy++) {
        for (int t = 0; t < 2; t++) {
            int px1 = mx + t;
            int px2 = mx + mw - 1 - t;
            int py = my + dy;
            if (py >= 0 && py < dispHeight) {
                if (px1 >= 0 && px1 < dispWidth) pixels[py * pitchPixels + px1] = borderColor;
                if (px2 >= 0 && px2 < dispWidth) pixels[py * pitchPixels + px2] = borderColor;
            }
        }
    }

    // Menu item placeholder lines
    uint32_t textColor = 0xFF000000;
    int textY = my + 10;
    for (int i = 0; i < 5 && textY + 20 < my + mh; i++) {
        int lineY = textY + 10;
        for (int dx = 10; dx < mw - 10; dx++) {
            int px = mx + dx;
            if (px >= 0 && px < dispWidth && lineY >= 0 && lineY < dispHeight) {
                if ((dx % 8) < 5) {
                    pixels[lineY * pitchPixels + px] = textColor;
                }
            }
        }
        textY += 30;
    }
}

void WorldRenderer::render() {
    if (!pharo::gDisplaySurface) return;

    // Throttle to ~60fps
    static auto lastRenderTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRenderTime);
    if (elapsed.count() < 16) {
        return;
    }
    lastRenderTime = now;

    int totalMorphsDrawn = 0;

    Oop world = memory_.findGlobal("World");
    if (world.isNil() || !world.isObject()) {
        return;
    }

    uint32_t* pixels = pharo::gDisplaySurface->pixels();
    int dispWidth = pharo::gDisplaySurface->width();
    int dispHeight = pharo::gDisplaySurface->height();

    // Clear to World's color
    Oop worldColor = memory_.fetchPointer(4, world);
    uint32_t worldColorARGB = extractColor(worldColor);

    for (int i = 0; i < dispWidth * dispHeight; i++) {
        pixels[i] = worldColorARGB;
    }

    // Get World's submorphs and render recursively
    Oop submorphs = memory_.fetchPointer(2, world);
    if (!submorphs.isNil() && submorphs.isObject()) {
        ObjectHeader* subHdr = submorphs.asObjectPtr();
        size_t numSubmorphs = subHdr->slotCount();

        // First pass: render all submorphs EXCEPT MenubarMorph (render it last so it's on top)
        Oop menubarMorph = Oop::nil();
        for (size_t i = 0; i < numSubmorphs; i++) {
            Oop submorph = subHdr->slotAt(i);
            std::string cn = getMorphClassName(submorph);
            if (cn == "MenubarMorph") {
                menubarMorph = submorph;
            } else {
                renderMorph(pixels, dispWidth, dispHeight, submorph, 0, static_cast<int>(i), totalMorphsDrawn);
            }
        }

        // Second pass: render MenubarMorph last so it's always on top
        if (!menubarMorph.isNil()) {
            renderMorph(pixels, dispWidth, dispHeight, menubarMorph, 0, 0, totalMorphsDrawn);
        }
    }

    // Redraw world menu if visible
    if (hasVisibleMenu_) {
        renderWorldMenuOverlay(pixels, dispWidth, dispHeight);
    }

    pharo::gDisplaySurface->update();
}

} // namespace pharo
