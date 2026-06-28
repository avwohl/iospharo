/*
 * Win32DisplaySurface.hpp - a real on-screen DisplaySurface for the Windows VM.
 *
 * Backs gDisplaySurface with a Win32 HWND instead of the in-memory
 * TestDisplaySurface.  Pharo writes the morphic World into pixels() (a 32bpp
 * XRGB buffer, which on little-endian Windows is BGRA byte order — exactly what a
 * BI_RGB 32bpp top-down DIB expects), and update() blits it to the window via
 * StretchDIBits.  Messages are pumped on update() so the window appears and stays
 * responsive while the World is rendering.  Enable with PHARO_GUI_WINDOW=1.
 *
 * This is the "present" half of the GUI; input (Win32 -> SDL event injection) is
 * a separate step.  See docs/deferred.md GUI section.
 */
#pragma once
#ifdef _WIN32

#include "DisplaySurface.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#include <cstdint>

namespace pharo {

class Win32DisplaySurface : public DisplaySurface {
public:
    Win32DisplaySurface(int w, int h)
        : width_(w), height_(h), pixels_(static_cast<size_t>(w) * h, 0xFF202020) {
        createWindow();
        ZeroMemory(&bmi_, sizeof(bmi_));
        bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi_.bmiHeader.biWidth = w;
        bmi_.bmiHeader.biHeight = -h;   // negative => top-down (row 0 at top)
        bmi_.bmiHeader.biPlanes = 1;
        bmi_.bmiHeader.biBitCount = 32;
        bmi_.bmiHeader.biCompression = BI_RGB;
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    int depth() const override { return 32; }
    uint32_t* pixels() override { return pixels_.data(); }
    size_t pitch() const override { return static_cast<size_t>(width_) * sizeof(uint32_t); }
    void invalidateRect(int, int, int, int) override {}

    void update() override {
        pumpMessages();
        if (hwnd_) {
            HDC dc = GetDC(hwnd_);
            if (dc) {
                StretchDIBits(dc, 0, 0, width_, height_, 0, 0, width_, height_,
                              pixels_.data(), &bmi_, DIB_RGB_COLORS, SRCCOPY);
                ReleaseDC(hwnd_, dc);
            }
        }
    }

    HWND hwnd() const { return hwnd_; }

private:
    int width_;
    int height_;
    std::vector<uint32_t> pixels_;
    HWND hwnd_ = nullptr;
    BITMAPINFO bmi_;

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcA(h, m, w, l);
    }

    void createWindow() {
        HINSTANCE inst = GetModuleHandleA(nullptr);
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = &Win32DisplaySurface::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = "PharoVMWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r = {0, 0, width_, height_};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowA("PharoVMWindow", "Pharo (Windows VM)",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, inst, nullptr);
        if (hwnd_) {
            ShowWindow(hwnd_, SW_SHOW);
            UpdateWindow(hwnd_);
        }
    }

    void pumpMessages() {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
};

} // namespace pharo

#endif // _WIN32
