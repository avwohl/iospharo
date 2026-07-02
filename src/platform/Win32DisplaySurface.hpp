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
#include "../vm/DebugVars.hpp"
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
#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>

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

    // Same time base as the harness's proven injectMouseClick (steady_clock ms).
    static int nowMs() {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return static_cast<int>(ms & 0x7FFFFFFF);
    }

    // Event encodings below MUST match vm_postMouseEvent/vm_postKeyEvent
    // (PlatformBridge.cpp) — the proven producer for stub_SDL_PollEvent.

    static int mkButtons(WPARAM w) {
        int b = 0;
        if (w & MK_LBUTTON) b |= 4;   // Red
        if (w & MK_RBUTTON) b |= 2;   // Yellow
        if (w & MK_MBUTTON) b |= 1;   // Blue
        return b;
    }

    static void pushWheel(int x, int y, int dx, int dy) {
        pharo::Event e;
        e.type = static_cast<int>(pharo::EventType::MouseWheel);
        e.timeStamp = nowMs();
        e.arg1 = x; e.arg2 = y;
        e.arg3 = dx; e.arg4 = dy;
        e.arg5 = curMods();
        e.windowIndex = 1;
        if (GET_DEBUG_BOOL(PHARO_WIN_EVENT_TRACE)) {
            fprintf(stderr, "[WIN-EVT] push wheel (%d,%d) d=(%d,%d)\n", x, y, dx, dy);
        }
        pharo::gEventQueue.push(e);
    }

    static void pushMouse(int x, int y, int buttons, int subtype) {
        pharo::Event e;
        e.type = static_cast<int>(pharo::EventType::Mouse);
        e.timeStamp = nowMs();
        e.arg1 = x; e.arg2 = y;
        e.arg3 = buttons;     // Red=4 (left), Yellow=2 (right), Blue=1 (middle)
        e.arg4 = curMods();
        e.arg5 = subtype;     // 1=down, 2=up, 3=move
        e.windowIndex = 1;
        if (GET_DEBUG_BOOL(PHARO_WIN_EVENT_TRACE)) {
            if (subtype == 3) {
                static unsigned moveCount = 0;
                if ((++moveCount & 63) == 0)
                    fprintf(stderr, "[WIN-EVT] push move #%u (%d,%d) buttons=%d\n",
                            moveCount, x, y, buttons);
            } else {
                fprintf(stderr, "[WIN-EVT] push mouse %s (%d,%d) buttons=%d mods=%d ts=%d\n",
                        subtype == 1 ? "DOWN" : "UP", x, y, buttons, e.arg4, e.timeStamp);
            }
        }
        pharo::gEventQueue.push(e);
    }

    // subtype: 0=down, 1=up, 2=keystroke (matches vm_postKeyEvent /
    // stub_SDL_PollEvent: arg1=charCode/keysym, arg2=subtype, arg3=mods,
    // arg4=SDL scancode).
    static void pushKey(int charCode, int scanCode, int subtype) {
        pharo::Event e;
        e.type = static_cast<int>(pharo::EventType::Keyboard);
        e.timeStamp = nowMs();
        e.arg1 = charCode;
        e.arg2 = subtype;
        e.arg3 = curMods();
        e.arg4 = scanCode;
        e.windowIndex = 1;
        if (GET_DEBUG_BOOL(PHARO_WIN_EVENT_TRACE)) {
            fprintf(stderr, "[WIN-EVT] push key subtype=%d char=%d scan=%d mods=%d\n",
                    subtype, charCode, scanCode, e.arg3);
        }
        pharo::gEventQueue.push(e);
    }

    // Map a Win32 virtual key to (SDL keycode, SDL/USB-HID scancode) — the
    // values stub_SDL_PollEvent feeds to the image's OSSDL2 key handling.
    // Letters/digits map to ASCII keycodes; navigation/function keys map to
    // scancode|0x40000000 exactly like SDL's SDL_SCANCODE_TO_KEYCODE.
    static void vkToSDL(WPARAM vk, LPARAM /*lParam*/, int& sym, int& scan) {
        sym = 0; scan = 0;
        if (vk >= 'A' && vk <= 'Z') {                       // HID: A=4..Z=29
            scan = 4 + static_cast<int>(vk - 'A');
            sym = static_cast<int>(vk - 'A') + 'a';         // SDL keycodes are lowercase
            return;
        }
        if (vk >= '1' && vk <= '9') {                       // HID: 1=30..9=38
            scan = 30 + static_cast<int>(vk - '1');
            sym = static_cast<int>(vk);
            return;
        }
        switch (vk) {
            case '0':        scan = 39;  sym = '0'; return;
            case VK_RETURN:  scan = 40;  sym = 13;  return;
            case VK_ESCAPE:  scan = 41;  sym = 27;  return;
            case VK_BACK:    scan = 42;  sym = 8;   return;
            case VK_TAB:     scan = 43;  sym = 9;   return;
            case VK_SPACE:   scan = 44;  sym = 32;  return;
            case VK_HOME:    scan = 74;  break;
            case VK_PRIOR:   scan = 75;  break;    // PageUp
            case VK_DELETE:  scan = 76;  sym = 127; return;
            case VK_END:     scan = 77;  break;
            case VK_NEXT:    scan = 78;  break;    // PageDown
            case VK_RIGHT:   scan = 79;  break;
            case VK_LEFT:    scan = 80;  break;
            case VK_DOWN:    scan = 81;  break;
            case VK_UP:      scan = 82;  break;
            case VK_SHIFT:   scan = 225; break;    // LShift
            case VK_CONTROL: scan = 224; break;    // LCtrl
            case VK_MENU:    scan = 226; break;    // LAlt
            default:
                if (vk >= VK_F1 && vk <= VK_F12) { scan = 58 + static_cast<int>(vk - VK_F1); break; }
                // Punctuation etc.: keydown sym from the untranslated char;
                // actual text arrives via WM_CHAR -> SDL_TEXTINPUT anyway.
                sym = static_cast<int>(MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR)) & 0x7FFFFFFF;
                return;
        }
        sym = 0x40000000 | scan;   // SDL_SCANCODE_TO_KEYCODE for non-printables
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
            // arg3 = buttons pressed AFTER the event (Pharo convention).
            // wParam's MK_* mask is exactly that for every mouse message —
            // e.g. WM_LBUTTONUP's wParam already has MK_LBUTTON cleared.
            case WM_MOUSEMOVE:   pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), mkButtons(w), 3); return 0;
            case WM_LBUTTONDOWN: SetCapture(h); pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), mkButtons(w), 1); return 0;
            case WM_LBUTTONUP:   ReleaseCapture(); pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), mkButtons(w), 2); return 0;
            case WM_RBUTTONDOWN: pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), mkButtons(w), 1); return 0;
            case WM_RBUTTONUP:   pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), mkButtons(w), 2); return 0;
            case WM_MBUTTONDOWN: pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), mkButtons(w), 1); return 0;
            case WM_MBUTTONUP:   pushMouse(GET_X_LPARAM(l), GET_Y_LPARAM(l), mkButtons(w), 2); return 0;
            case WM_MOUSEWHEEL: {
                POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };  // wheel gives SCREEN coords
                ScreenToClient(h, &pt);
                pushWheel(pt.x, pt.y, 0, GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
                return 0;
            }
            case WM_CHAR:        pushKey(static_cast<int>(w), 0, 2); return 0;
            case WM_KEYDOWN:
            case WM_KEYUP: {
                int sym, scan;
                vkToSDL(w, l, sym, scan);
                pushKey(sym, scan, m == WM_KEYDOWN ? 0 : 1);
                return 0;
            }
        }
        return DefWindowProcA(h, m, w, l);
    }
};

} // namespace pharo

#endif // _WIN32
