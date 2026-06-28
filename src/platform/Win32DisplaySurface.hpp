/*
 * Win32DisplaySurface.hpp - a real on-screen DisplaySurface for the Windows VM.
 *
 * Backs gDisplaySurface with a Win32 HWND instead of the in-memory
 * TestDisplaySurface.  Pharo writes the morphic World into pixels() (a 32bpp XRGB
 * buffer = BGRA byte order on little-endian, matching a BI_RGB 32bpp top-down DIB),
 * and the window is blitted via GDI StretchDIBits.
 *
 * The window lives on a DEDICATED THREAD running a GetMessage loop, so it pumps
 * messages (and stays responsive) continuously — independent of how often the VM
 * renders.  update() (VM thread) just InvalidateRect's to request a repaint; the
 * window thread paints on WM_PAINT.  The window proc translates mouse/keyboard
 * input into pharo::Event and pushes to gEventQueue (-> stub_SDL_PollEvent ->
 * the image), so the GUI is interactive.  Enable with PHARO_GUI_WINDOW=1.
 */
#pragma once
#ifdef _WIN32

#include "DisplaySurface.hpp"
#include "EventQueue.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>

namespace pharo {

class Win32DisplaySurface : public DisplaySurface {
public:
    Win32DisplaySurface(int w, int h)
        : width_(w), height_(h), pixels_(static_cast<size_t>(w) * h, 0xFF202020) {
        ZeroMemory(&bmi_, sizeof(bmi_));
        bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi_.bmiHeader.biWidth = w;
        bmi_.bmiHeader.biHeight = -h;   // top-down (row 0 at top)
        bmi_.bmiHeader.biPlanes = 1;
        bmi_.bmiHeader.biBitCount = 32;
        bmi_.bmiHeader.biCompression = BI_RGB;
        thread_ = std::thread(&Win32DisplaySurface::windowThread, this);
        // Wait until the window exists so update()/hwnd() are usable on return.
        while (!ready_.load(std::memory_order_acquire)) Sleep(1);
    }

    ~Win32DisplaySurface() override {
        if (hwnd_) PostMessageA(hwnd_, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

    int width() const override { return width_; }
    int height() const override { return height_; }
    int depth() const override { return 32; }
    uint32_t* pixels() override { return pixels_.data(); }
    size_t pitch() const override { return static_cast<size_t>(width_) * sizeof(uint32_t); }
    void invalidateRect(int, int, int, int) override { requestPaint(); }
    void update() override { requestPaint(); }

    HWND hwnd() const { return hwnd_; }

private:
    int width_;
    int height_;
    std::vector<uint32_t> pixels_;
    BITMAPINFO bmi_;
    HWND hwnd_ = nullptr;
    std::thread thread_;
    std::atomic<bool> ready_{false};

    void requestPaint() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);  // posts WM_PAINT to the window thread
    }

    // ---- window thread: owns the HWND + the message loop ----
    void windowThread() {
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
                              nullptr, nullptr, inst, this);
        if (hwnd_) {
            SetWindowLongPtrA(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(hwnd_, SW_SHOW);
            UpdateWindow(hwnd_);
        }
        ready_.store(true, std::memory_order_release);
        MSG msg;
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    static Win32DisplaySurface* self(HWND h) {
        return reinterpret_cast<Win32DisplaySurface*>(GetWindowLongPtrA(h, GWLP_USERDATA));
    }

    static int curMods() {
        int mods = 0;
        if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= pharo::ShiftKey;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= pharo::CtrlKey;
        if (GetKeyState(VK_MENU)    & 0x8000) mods |= pharo::AltKey;
        return mods;
    }

    static void pushMouse(int x, int y, int buttons, int subtype) {
        pharo::Event e;
        e.type = static_cast<int>(pharo::EventType::Mouse);
        e.arg1 = x; e.arg2 = y;
        e.arg3 = buttons;     // Red=4 (left), Yellow=2 (right), Blue=1 (middle)
        e.arg4 = curMods();
        e.arg5 = subtype;     // 1=down, 2=up, 3=move
        e.windowIndex = 1;
        pharo::gEventQueue.push(e);
    }

    static void pushKey(int code, int subtype) {
        pharo::Event e;
        e.type = static_cast<int>(pharo::EventType::Keyboard);
        e.arg1 = code;
        e.arg3 = curMods();
        e.arg4 = subtype;     // 1=down, 2=up, 3=char
        e.windowIndex = 1;
        pharo::gEventQueue.push(e);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
            case WM_PAINT: {
                Win32DisplaySurface* s = self(h);
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                if (s && dc) {
                    StretchDIBits(dc, 0, 0, s->width_, s->height_, 0, 0, s->width_, s->height_,
                                  s->pixels_.data(), &s->bmi_, DIB_RGB_COLORS, SRCCOPY);
                }
                EndPaint(h, &ps);
                return 0;
            }
            case WM_ERASEBKGND: return 1;   // we fully paint; skip background erase (no flicker)
            case WM_DESTROY: PostQuitMessage(0); return 0;
            case WM_MOUSEMOVE: {
                int b = 0;
                if (w & MK_LBUTTON) b |= 4;
                if (w & MK_RBUTTON) b |= 2;
                if (w & MK_MBUTTON) b |= 1;
                pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), b, 3);
                return 0;
            }
            case WM_LBUTTONDOWN: SetCapture(h); pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), 4, 1); return 0;
            case WM_LBUTTONUP:   ReleaseCapture(); pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), 0, 2); return 0;
            case WM_RBUTTONDOWN: pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), 2, 1); return 0;
            case WM_RBUTTONUP:   pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), 0, 2); return 0;
            case WM_MBUTTONDOWN: pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), 1, 1); return 0;
            case WM_MBUTTONUP:   pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), 0, 2); return 0;
            case WM_CHAR:        pushKey(static_cast<int>(w), 3); return 0;
            case WM_KEYDOWN:     pushKey(static_cast<int>(w), 1); return 0;
            case WM_KEYUP:       pushKey(static_cast<int>(w), 2); return 0;
        }
        return DefWindowProcA(h, m, w, l);
    }
};

} // namespace pharo

#endif // _WIN32
