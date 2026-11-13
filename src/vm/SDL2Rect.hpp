/*
 * SDL2Rect.hpp - rectangle, point and line-clip functions of SDL_rect.h.
 *
 * One template serves the integer and the float versions.  The rules follow
 * the SDL 2.x documentation: a rectangle with a width or height of zero or
 * less is empty; an intersection or union with an empty rectangle follows
 * the special cases of the manual; a line is clipped with the
 * Cohen-Sutherland method in the integer arithmetic of the integer version.
 *
 * Copyright (c) 2025-2026 Aaron Wohl. Licensed under the MIT License.
 */
#pragma once

#include "SDL2Sys.hpp"

namespace pharo {
namespace sdl2 {
namespace rect {

template <class T> struct PointT { T x, y; };
template <class T> struct RectT { T x, y, w, h; };

template <class T> inline bool isEmpty(const RectT<T>* r) { return !r || r->w <= 0 || r->h <= 0; }

template <class T>
inline int hasIntersection(const RectT<T>* a, const RectT<T>* b) {
    if (!a) { sys::invalidParam("A"); return 0; }
    if (!b) { sys::invalidParam("B"); return 0; }
    if (isEmpty(a) || isEmpty(b)) return 0;
    T lo = std::max(a->x, b->x), hi = std::min(a->x + a->w, b->x + b->w);
    if (hi <= lo) return 0;
    lo = std::max(a->y, b->y);
    hi = std::min(a->y + a->h, b->y + b->h);
    return hi > lo ? 1 : 0;
}

template <class T>
inline int intersectRect(const RectT<T>* a, const RectT<T>* b, RectT<T>* result) {
    if (!a) { sys::invalidParam("A"); return 0; }
    if (!b) { sys::invalidParam("B"); return 0; }
    if (!result) { sys::invalidParam("result"); return 0; }
    if (isEmpty(a) || isEmpty(b)) { result->w = 0; result->h = 0; return 0; }
    T lo = std::max(a->x, b->x), hi = std::min(a->x + a->w, b->x + b->w);
    result->x = lo;
    result->w = hi - lo;
    lo = std::max(a->y, b->y);
    hi = std::min(a->y + a->h, b->y + b->h);
    result->y = lo;
    result->h = hi - lo;
    return isEmpty(result) ? 0 : 1;
}

template <class T>
inline void unionRect(const RectT<T>* a, const RectT<T>* b, RectT<T>* result) {
    if (!a) { sys::invalidParam("A"); return; }
    if (!b) { sys::invalidParam("B"); return; }
    if (!result) { sys::invalidParam("result"); return; }
    if (isEmpty(a)) {
        if (isEmpty(b)) *result = RectT<T>{ 0, 0, 0, 0 };
        else *result = *b;
        return;
    }
    if (isEmpty(b)) { *result = *a; return; }
    T lo = std::min(a->x, b->x), hi = std::max(a->x + a->w, b->x + b->w);
    result->x = lo;
    result->w = hi - lo;
    lo = std::min(a->y, b->y);
    hi = std::max(a->y + a->h, b->y + b->h);
    result->y = lo;
    result->h = hi - lo;
}

// The smallest rectangle that holds the points inside `clip` (all points when
// `clip` is null).  With no result pointer only the answer is computed.
template <class T>
inline int enclosePoints(const PointT<T>* points, int count, const RectT<T>* clip, RectT<T>* result) {
    if (!points) { sys::invalidParam("points"); return 0; }
    if (count < 1) { sys::invalidParam("count"); return 0; }
    T minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool added = false;
    if (clip) {
        if (isEmpty(clip)) return 0;
        const T cx0 = clip->x, cy0 = clip->y, cx1 = clip->x + clip->w - 1, cy1 = clip->y + clip->h - 1;
        for (int i = 0; i < count; i++) {
            const T x = points[i].x, y = points[i].y;
            if (x < cx0 || x > cx1 || y < cy0 || y > cy1) continue;
            if (!added) {
                if (!result) return 1;
                minx = maxx = x;
                miny = maxy = y;
                added = true;
                continue;
            }
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);
        }
        if (!added) return 0;
    } else {
        if (!result) return 1;
        minx = maxx = points[0].x;
        miny = maxy = points[0].y;
        for (int i = 1; i < count; i++) {
            minx = std::min(minx, points[i].x); maxx = std::max(maxx, points[i].x);
            miny = std::min(miny, points[i].y); maxy = std::max(maxy, points[i].y);
        }
    }
    if (result) {
        result->x = minx;
        result->y = miny;
        result->w = (maxx - minx) + 1;
        result->h = (maxy - miny) + 1;
    }
    return 1;
}

enum OutCode : int { kCodeBottom = 1, kCodeTop = 2, kCodeLeft = 4, kCodeRight = 8 };

template <class T>
inline int outCode(const RectT<T>& r, T x, T y) {
    int code = 0;
    if (y < r.y) code |= kCodeTop; else if (y >= r.y + r.h) code |= kCodeBottom;
    if (x < r.x) code |= kCodeLeft; else if (x >= r.x + r.w) code |= kCodeRight;
    return code;
}

// Clip the line (x1,y1)-(x2,y2) to the rectangle.  Zero when no part of the
// line is inside.  The integer version keeps the integer arithmetic of the
// division, so a clipped line can differ in slope from the original.
template <class T>
inline int intersectRectAndLine(const RectT<T>* r, T* X1, T* Y1, T* X2, T* Y2) {
    if (!r) { sys::invalidParam("rect"); return 0; }
    if (!X1) { sys::invalidParam("X1"); return 0; }
    if (!Y1) { sys::invalidParam("Y1"); return 0; }
    if (!X2) { sys::invalidParam("X2"); return 0; }
    if (!Y2) { sys::invalidParam("Y2"); return 0; }
    if (isEmpty(r)) return 0;
    T x1 = *X1, y1 = *Y1, x2 = *X2, y2 = *Y2, x = 0, y = 0;
    const T rx1 = r->x, ry1 = r->y, rx2 = r->x + r->w - 1, ry2 = r->y + r->h - 1;
    if (x1 >= rx1 && x1 <= rx2 && x2 >= rx1 && x2 <= rx2 && y1 >= ry1 && y1 <= ry2 && y2 >= ry1 && y2 <= ry2) return 1;
    if ((x1 < rx1 && x2 < rx1) || (x1 > rx2 && x2 > rx2) || (y1 < ry1 && y2 < ry1) || (y1 > ry2 && y2 > ry2)) return 0;
    if (y1 == y2) {
        if (x1 < rx1) *X1 = rx1; else if (x1 > rx2) *X1 = rx2;
        if (x2 < rx1) *X2 = rx1; else if (x2 > rx2) *X2 = rx2;
        return 1;
    }
    if (x1 == x2) {
        if (y1 < ry1) *Y1 = ry1; else if (y1 > ry2) *Y1 = ry2;
        if (y2 < ry1) *Y2 = ry1; else if (y2 > ry2) *Y2 = ry2;
        return 1;
    }
    int code1 = outCode(*r, x1, y1), code2 = outCode(*r, x2, y2);
    while (code1 || code2) {
        if (code1 & code2) return 0;
        const bool first = code1 != 0;
        const int code = first ? code1 : code2;
        if (code & kCodeTop) { y = ry1; x = x1 + ((x2 - x1) * (y - y1)) / (y2 - y1); }
        else if (code & kCodeBottom) { y = ry2; x = x1 + ((x2 - x1) * (y - y1)) / (y2 - y1); }
        else if (code & kCodeLeft) { x = rx1; y = y1 + ((y2 - y1) * (x - x1)) / (x2 - x1); }
        else if (code & kCodeRight) { x = rx2; y = y1 + ((y2 - y1) * (x - x1)) / (x2 - x1); }
        if (first) { x1 = x; y1 = y; code1 = outCode(*r, x, y); }
        else { x2 = x; y2 = y; code2 = outCode(*r, x, y); }
    }
    *X1 = x1; *Y1 = y1; *X2 = x2; *Y2 = y2;
    return 1;
}

} // namespace rect
} // namespace sdl2
} // namespace pharo
