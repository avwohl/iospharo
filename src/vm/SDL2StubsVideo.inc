// SDL2StubsVideo.inc - window, display, keyboard, mouse, clipboard and the
// video functions of features that the host does not have.
//
// The host window is a view of the app, so the answers follow SDL on a video
// driver that lacks the feature: the window functions that the host cannot do
// (opacity, borders, modal windows, flash, gamma) fail with SDL's own
// "That operation is not supported".  OpenGL, Vulkan, Metal and shaped
// windows answer the error SDL gives when the driver has no such support.
// The text of each answer was checked against a real libSDL2 on a driver
// without the feature.
//
// Included by FFI.cpp inside namespace pharo::ffi.

#include "SDL2KeyTable.inc"

static const char kUnsupported[] = "That operation is not supported";

static SDLWindowState* findWindow(void* w) {
    auto it = sWindows.find(w);
    return it == sWindows.end() ? nullptr : &it->second;
}

// The first line of nearly every window function: an unknown handle is "Invalid window".
#define WINDOW_OR(retval, w, var) \
    SDLWindowState* var = findWindow(w); \
    if (!var) { sdl2::sys::setError("Invalid window"); return retval; }

// ----- Window size limits, pixel size, data, grab -----

static void stub_SDL_GetWindowMinimumSize(void* w, int* ow, int* oh) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return; }
    if (ow) *ow = s->minW;
    if (oh) *oh = s->minH;
}

static void stub_SDL_GetWindowMaximumSize(void* w, int* ow, int* oh) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return; }
    if (ow) *ow = s->maxW;
    if (oh) *oh = s->maxH;
}

static void stub_SDL_SetWindowMinimumSize(void* w, int minW, int minH) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return; }
    if (minW <= 0) { sdl2::sys::invalidParam("min_w"); return; }
    if (minH <= 0) { sdl2::sys::invalidParam("min_h"); return; }
    if ((s->maxW && minW > s->maxW) || (s->maxH && minH > s->maxH)) {
        sdl2::sys::setError("SDL_SetWindowMinimumSize(): Tried to set minimum size larger than maximum size");
        return;
    }
    s->minW = minW;
    s->minH = minH;
    // A window smaller than its new minimum grows.
    if (s->width < minW || s->height < minH) stub_SDL_SetWindowSize(w, std::max(s->width, minW), std::max(s->height, minH));
}

static void stub_SDL_SetWindowMaximumSize(void* w, int maxW, int maxH) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return; }
    if (maxW <= 0) { sdl2::sys::invalidParam("max_w"); return; }
    if (maxH <= 0) { sdl2::sys::invalidParam("max_h"); return; }
    if (maxW < s->minW || maxH < s->minH) {
        sdl2::sys::setError("SDL_SetWindowMaximumSize(): Tried to set maximum size smaller than minimum size");
        return;
    }
    s->maxW = maxW;
    s->maxH = maxH;
    if (s->width > maxW || s->height > maxH) stub_SDL_SetWindowSize(w, std::min(s->width, maxW), std::min(s->height, maxH));
}

// The pixel size of a window equals its size: the app view has no separate pixel scale.
static void stub_SDL_GetWindowSizeInPixels(void* w, int* ow, int* oh) {
    if (!findWindow(w)) { sdl2::sys::setError("Invalid window"); return; }
    stub_SDL_GetWindowSize(w, ow, oh);
}

static void* stub_SDL_SetWindowData(void* w, const char* name, void* userdata) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return nullptr; }
    if (!name || !name[0]) { sdl2::sys::invalidParam("name"); return nullptr; }
    void* previous = nullptr;
    auto it = s->data.find(name);
    if (it != s->data.end()) previous = it->second;
    if (userdata) s->data[name] = userdata; else s->data.erase(name);
    return previous;
}

static void* stub_SDL_GetWindowData(void* w, const char* name) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return nullptr; }
    if (!name || !name[0]) { sdl2::sys::invalidParam("name"); return nullptr; }
    auto it = s->data.find(name);
    return it == s->data.end() ? nullptr : it->second;
}

static void* sGrabbedWindow = nullptr;

static void stub_SDL_SetWindowGrab(void* w, int grabbed) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return; }
    // SDL_SetWindowGrab grabs the mouse; it grabs the keyboard too only when the hint asks for it.
    s->mouseGrab = grabbed != 0;
    s->keyboardGrab = grabbed != 0 && sdl2::base::getHintBoolean("SDL_GRAB_KEYBOARD", 0);
    if (grabbed) sGrabbedWindow = w; else if (sGrabbedWindow == w) sGrabbedWindow = nullptr;
}
static void stub_SDL_SetWindowMouseGrab(void* w, int grabbed) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return; }
    s->mouseGrab = grabbed != 0;
    if (grabbed) sGrabbedWindow = w; else if (sGrabbedWindow == w && !s->keyboardGrab) sGrabbedWindow = nullptr;
}
static void stub_SDL_SetWindowKeyboardGrab(void* w, int grabbed) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return; }
    s->keyboardGrab = grabbed != 0;
    if (grabbed) sGrabbedWindow = w; else if (sGrabbedWindow == w && !s->mouseGrab) sGrabbedWindow = nullptr;
}
static int stub_SDL_GetWindowGrab(void* w) {
    SDLWindowState* s = findWindow(w);
    if (!s) return sdl2::sys::setError("Invalid window"), 0;
    return (s->mouseGrab || s->keyboardGrab) ? 1 : 0;
}
static int stub_SDL_GetWindowMouseGrab(void* w) {
    SDLWindowState* s = findWindow(w);
    if (!s) return sdl2::sys::setError("Invalid window"), 0;
    return s->mouseGrab ? 1 : 0;
}
static int stub_SDL_GetWindowKeyboardGrab(void* w) {
    SDLWindowState* s = findWindow(w);
    if (!s) return sdl2::sys::setError("Invalid window"), 0;
    return s->keyboardGrab ? 1 : 0;
}
static void* stub_SDL_GetGrabbedWindow() { return sGrabbedWindow; }

static int stub_SDL_SetWindowMouseRect(void* w, const void* rect) {
    SDLWindowState* s = findWindow(w);
    if (!s) return sdl2::sys::setError("Invalid window");
    s->hasMouseRect = rect != nullptr;
    if (rect) s->mouseRect = *static_cast<const sdl2::Rect*>(rect);
    return 0;
}
static const void* stub_SDL_GetWindowMouseRect(void* w) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return nullptr; }
    return s->hasMouseRect ? &s->mouseRect : nullptr;
}

// ----- Window features the host cannot give -----

static int stub_SDL_GetWindowBordersSize(void* w, int* top, int* left, int* bottom, int* right) {
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window");
    if (top) *top = 0;
    if (left) *left = 0;
    if (bottom) *bottom = 0;
    if (right) *right = 0;
    return sdl2::sys::setError(kUnsupported);
}
static int stub_SDL_SetWindowOpacity(void* w, float opacity) {
    (void)opacity;
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window");
    return sdl2::sys::setError(kUnsupported);
}
static int stub_SDL_GetWindowOpacity(void* w, float* out) {
    SDLWindowState* s = findWindow(w);
    if (!s) return sdl2::sys::setError("Invalid window");
    if (out) *out = s->opacity;
    return 0;
}
static int stub_SDL_SetWindowModalFor(void* modal, void* parent) {
    if (!findWindow(modal) || !findWindow(parent)) return sdl2::sys::setError("Invalid window");
    return sdl2::sys::setError(kUnsupported);
}
static int stub_SDL_FlashWindow(void* w, int operation) {
    (void)operation;
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window");
    return sdl2::sys::setError(kUnsupported);
}
static void stub_SDL_SetWindowAlwaysOnTop(void* w, int onTop) {
    (void)onTop;
    if (!findWindow(w)) sdl2::sys::setError("Invalid window");
    // A driver without this feature leaves the window as it is.
}
static void* stub_SDL_GetWindowICCProfile(void* w, size_t* size) {
    if (size) *size = 0;
    (void)w;
    sdl2::sys::setError(kUnsupported);
    return nullptr;
}
static void* stub_SDL_CreateWindowFrom(const void* nativeHandle) {
    (void)nativeHandle;
    sdl2::sys::setError(kUnsupported);
    return nullptr;
}
static float stub_SDL_GetWindowBrightness(void* w) {
    SDLWindowState* s = findWindow(w);
    if (!s) { sdl2::sys::setError("Invalid window"); return 1.0f; }
    return s->brightness;
}
static int stub_SDL_SetWindowBrightness(void* w, float brightness) {
    (void)brightness;
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window");
    return sdl2::sys::setError(kUnsupported);   // brightness is a gamma ramp
}
static int stub_SDL_SetWindowGammaRamp(void* w, const uint16_t* r, const uint16_t* g, const uint16_t* b) {
    (void)r; (void)g; (void)b;
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window");
    return sdl2::sys::setError(kUnsupported);
}
// The ramp of a window that cannot change it is the identity ramp.
static int stub_SDL_GetWindowGammaRamp(void* w, uint16_t* r, uint16_t* g, uint16_t* b) {
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window");
    for (int i = 0; i < 256; i++) {
        const uint16_t v = static_cast<uint16_t>((i << 8) | i);
        if (r) r[i] = v;
        if (g) g[i] = v;
        if (b) b[i] = v;
    }
    return 0;
}

// ----- Display queries -----

static int stub_SDL_SetWindowDisplayMode(void* w, const void* mode) {
    (void)mode;
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window");
    return 0;
}
static int stub_SDL_GetDesktopDisplayMode(int displayIndex, void* mode) { return stub_SDL_GetCurrentDisplayMode(displayIndex, mode); }

static void* stub_SDL_GetClosestDisplayMode(int displayIndex, const void* mode, void* closest) {
    if (!mode) { sdl2::sys::invalidParam("mode/closest"); return nullptr; }
    if (!closest) { sdl2::sys::invalidParam("mode/closest"); return nullptr; }
    if (stub_SDL_GetCurrentDisplayMode(displayIndex, closest) < 0) return nullptr;
    return closest;
}

static const char* stub_SDL_GetDisplayName(int displayIndex) {
    if (displayIndex < 0 || displayIndex >= stub_SDL_GetNumVideoDisplays()) {
        sdl2::sys::setError("displayIndex must be in the range 0 - 0");
        return nullptr;
    }
    return "Pharo display";
}

// The display has no rotation sensor in the VM.
static int stub_SDL_GetDisplayOrientation(int displayIndex) {
    if (displayIndex < 0 || displayIndex >= stub_SDL_GetNumVideoDisplays()) return sdl2::sys::setError("displayIndex must be in the range 0 - 0");
    return 0;   // SDL_ORIENTATION_UNKNOWN
}

static uint32_t stub_SDL_GetWindowPixelFormat(void* w) {
    if (!findWindow(w)) { sdl2::sys::setError("Invalid window"); return 0; }
    return sdl2::kFormatXRGB8888;   // the format of the one display mode
}

static int stub_SDL_GetPointDisplayIndex(const void* point) {
    if (!point) return sdl2::sys::invalidParam("point");
    return 0;   // one display holds every point
}
static int stub_SDL_GetRectDisplayIndex(const void* rect) {
    if (!rect) return sdl2::sys::invalidParam("rect");
    return 0;
}

static int stub_SDL_GetNumVideoDrivers() { return 1; }
static const char* stub_SDL_GetVideoDriver(int index) { return index == 0 ? stub_SDL_GetCurrentVideoDriver() : nullptr; }

// The screen saver starts disabled: SDL turns it off when the video system starts.
static void stub_SDL_EnableScreenSaver() { sScreenSaverEnabled = true; }
static int stub_SDL_IsScreenSaverEnabled() { return sScreenSaverEnabled ? 1 : 0; }

// ----- OpenGL, Vulkan, Metal, shaped windows, message boxes -----

static std::string noSupportText(const char* what) {
    return std::string("No dynamic ") + what + " support in current SDL video driver (" + stub_SDL_GetCurrentVideoDriver() + ")";
}
static int stub_SDL_GL_LoadLibrary(const char* path) { (void)path; return sdl2::sys::setError(noSupportText("OpenGL").c_str()); }
static void stub_SDL_GL_UnloadLibrary() {}
static int stub_SDL_GL_ExtensionSupported(const char* name) {
    // SDL rejects an empty name or a name with a space without an error, then
    // asks the GL driver, which does not exist here.
    if (!name || !*name || strchr(name, ' ')) return 0;
    sdl2::sys::setError(noSupportText("GL").c_str());
    return 0;
}
static void stub_SDL_GL_ResetAttributes() {
    std::lock_guard<std::mutex> lk(sGLAttrMutex);
    glAttributesReset();
}
static void* stub_SDL_GL_GetCurrentContext() { return nullptr; }
static void* stub_SDL_GL_GetCurrentWindow() { return nullptr; }
static int stub_SDL_GL_GetSwapInterval() { return 0; }
static int stub_SDL_GL_SetSwapInterval(int interval) { (void)interval; return sdl2::sys::setError("No OpenGL context has been made current"); }

static int stub_SDL_Vulkan_LoadLibrary(const char* path) { (void)path; return sdl2::sys::setError(noSupportText("Vulkan").c_str()); }
static void stub_SDL_Vulkan_UnloadLibrary() {}
static void* stub_SDL_Vulkan_GetVkGetInstanceProcAddr() { sdl2::sys::setError("No Vulkan loader has been loaded"); return nullptr; }
static int stub_SDL_Vulkan_GetInstanceExtensions(void* w, unsigned* count, const char** names) {
    (void)names;
    if (!count) return sdl2::sys::invalidParam("count"), 0;
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window"), 0;
    sdl2::sys::setError("The specified window isn't a Vulkan window");
    return 0;
}
static int stub_SDL_Vulkan_CreateSurface(void* w, void* instance, void* surface) {
    (void)instance; (void)surface;
    if (!findWindow(w)) return sdl2::sys::setError("Invalid window"), 0;
    sdl2::sys::setError("The specified window isn't a Vulkan window");
    return 0;
}
static void stub_SDL_Vulkan_GetDrawableSize(void* w, int* ow, int* oh) { stub_SDL_GetWindowSize(w, ow, oh); }

static void* stub_SDL_Metal_CreateView(void* w) {
    if (!findWindow(w)) { sdl2::sys::setError("Invalid window"); return nullptr; }
    sdl2::sys::setError("The specified window isn't a Metal window");
    return nullptr;
}
static void stub_SDL_Metal_DestroyView(void* view) { (void)view; }
static void* stub_SDL_Metal_GetLayer(void* view) { (void)view; sdl2::sys::setError("Metal is not supported."); return nullptr; }
static void stub_SDL_Metal_GetDrawableSize(void* w, int* ow, int* oh) { stub_SDL_GetWindowSize(w, ow, oh); }

// A window made by SDL_CreateWindow is never shaped.  SDL answers -1
// (SDL_NONSHAPEABLE_WINDOW) to the calls on such a window.
static void* stub_SDL_CreateShapedWindow(const char* title, unsigned x, unsigned y, unsigned w, unsigned h, uint32_t flags) {
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    return nullptr;
}
static int stub_SDL_IsShapedWindow(const void* w) { (void)w; return 0; }
static int stub_SDL_SetWindowShape(void* w, void* surface, void* mode) { (void)w; (void)surface; (void)mode; return -1; }
static int stub_SDL_GetShapedWindowMode(void* w, void* mode) { (void)w; (void)mode; return -1; }

static int stub_SDL_ShowSimpleMessageBox(uint32_t flags, const char* title, const char* message, void* w) {
    (void)flags; (void)title; (void)message; (void)w;
    return sdl2::sys::setError("No message system available");
}
static int stub_SDL_ShowMessageBox(const void* data, int* buttonId) {
    (void)buttonId;
    if (!data) return sdl2::sys::invalidParam("messageboxdata");
    return sdl2::sys::setError("No message system available");
}

// ----- Keyboard (SDL_keyboard.h) -----

constexpr int32_t kScancodeMask = 1 << 30;   // SDLK_SCANCODE_MASK

static const char* stub_SDL_GetScancodeName(int scancode) {
    if (scancode < 0 || scancode >= kScancodeCount) { sdl2::sys::invalidParam("scancode"); return ""; }
    return kScancodeNames[scancode] ? kScancodeNames[scancode] : "";
}

static int stub_SDL_GetScancodeFromName(const char* name) {
    if (!name || !*name) { sdl2::sys::invalidParam("name"); return 0; }
    for (int i = 0; i < kScancodeCount; i++)
        if (kScancodeNames[i] && strcasecmp(name, kScancodeNames[i]) == 0) return i;
    sdl2::sys::invalidParam("name");
    return 0;
}

static int32_t stub_SDL_GetKeyFromScancode(int scancode) {
    if (scancode < 0 || scancode >= kScancodeCount) { sdl2::sys::invalidParam("scancode"); return 0; }
    return kDefaultKeymap[scancode];
}

static int stub_SDL_GetScancodeFromKey(int32_t key) {
    for (int i = 0; i < kScancodeCount; i++) if (kDefaultKeymap[i] == key) return i;
    return 0;
}

static const char* stub_SDL_GetKeyName(int32_t key) {
    static thread_local char name[8];
    constexpr int32_t kReturn = '\r', kEscape = 27, kBackspace = '\b', kTab = '\t', kSpace = ' ', kDelete = 127;
    constexpr int kScReturn = 40, kScEscape = 41, kScBackspace = 42, kScTab = 43, kScSpace = 44, kScDelete = 76;
    if (key & kScancodeMask) return stub_SDL_GetScancodeName(key & ~kScancodeMask);
    switch (key) {
    case kReturn: return stub_SDL_GetScancodeName(kScReturn);
    case kEscape: return stub_SDL_GetScancodeName(kScEscape);
    case kBackspace: return stub_SDL_GetScancodeName(kScBackspace);
    case kTab: return stub_SDL_GetScancodeName(kScTab);
    case kSpace: return stub_SDL_GetScancodeName(kScSpace);
    case kDelete: return stub_SDL_GetScancodeName(kScDelete);
    default:
        if (key >= 'a' && key <= 'z') key -= 32;
        sdl2::utf::encode(static_cast<uint32_t>(key), name);
        return name;
    }
}

// A single UTF-8 character names itself; any other text is a scancode name.
static int32_t stub_SDL_GetKeyFromName(const char* name) {
    constexpr unsigned char kUpperA = 'A', kUpperZ = 'Z';
    constexpr int kCaseOffset = 'a' - 'A';
    if (!name) return 0;
    const unsigned char first = static_cast<unsigned char>(*name);
    const size_t len = strlen(name);
    if (first >= sdl2::utf::kLead2Tag) {
        const size_t n = sdl2::utf::sequenceLength(first);
        if (n == 0 || len != n) return 0;
        uint32_t cp = sdl2::utf::leadPayload(first, n);
        for (size_t i = 1; i < n; i++) cp = (cp << sdl2::utf::kPayloadBits) | (static_cast<unsigned char>(name[i]) & sdl2::utf::kPayloadMask);
        return static_cast<int32_t>(cp);
    }
    if (len == 1) return (first >= kUpperA && first <= kUpperZ) ? first + kCaseOffset : first;
    return kDefaultKeymap[stub_SDL_GetScancodeFromName(name)];
}

static const uint8_t* stub_SDL_GetKeyboardState(int* numkeys) {
    if (numkeys) *numkeys = kScancodeCount;
    return sKeyState;
}
static void* stub_SDL_GetKeyboardFocus() { return sMainWindow; }
static void stub_SDL_ResetKeyboard() { memset(sKeyState, 0, sizeof(sKeyState)); }
static void stub_SDL_ClearComposition() {}
static int stub_SDL_HasScreenKeyboardSupport() {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    return 1;
#else
    return 0;
#endif
}
static int stub_SDL_IsScreenKeyboardShown(void* w) {
    (void)w;
    return stub_SDL_HasScreenKeyboardSupport() && sTextInputActive ? 1 : 0;
}
static int stub_SDL_IsTextInputShown() { return 0; }

// ----- Mouse (SDL_mouse.h) -----

static void* sCurrentCursor = nullptr;
static void* sDefaultCursor = nullptr;

static void* stub_SDL_GetCursor() { return sCurrentCursor ? sCurrentCursor : sDefaultCursor; }
static void* stub_SDL_GetDefaultCursor() {
    constexpr int kSystemCursorArrow = 0;
    if (!sDefaultCursor) sDefaultCursor = stub_SDL_CreateSystemCursor(kSystemCursorArrow);
    return sDefaultCursor;
}
static void* stub_SDL_GetMouseFocus() { return sMainWindow; }
static int stub_SDL_GetRelativeMouseMode() { return sRelativeMouseMode ? 1 : 0; }
static uint32_t stub_SDL_GetRelativeMouseState(int* x, int* y) {
    if (x) *x = sRelMouseDx;
    if (y) *y = sRelMouseDy;
    sRelMouseDx = sRelMouseDy = 0;
    return sMouseButtons;
}
static int stub_SDL_CaptureMouse(int enabled) { (void)enabled; return sdl2::sys::setError(kUnsupported); }
static int stub_SDL_WarpMouseGlobal(int x, int y) { (void)x; (void)y; return sdl2::sys::setError(kUnsupported); }

// SDL_WarpMouseInWindow: with no native warp, SDL moves its own mouse state and
// sends a mouse motion event for the new position.
static void stub_SDL_WarpMouseInWindow(void* w, int x, int y) {
    if (w && !findWindow(w)) { sdl2::sys::setError("Invalid window"); return; }
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.motion.type = SDL_MOUSEMOTION;
    e.motion.timestamp = stub_SDL_GetTicks();
    e.motion.windowID = stub_SDL_GetWindowID(w ? w : sMainWindow);
    e.motion.state = sMouseButtons;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = x - sMouseX;
    e.motion.yrel = y - sMouseY;
    sRelMouseDx += e.motion.xrel;
    sRelMouseDy += e.motion.yrel;
    sMouseX = x;
    sMouseY = y;
    sdl2::ev::Event queued;
    memcpy(&queued, &e, sizeof(queued));
    sdl2::ev::enqueue(queued);
}

// ----- Clipboard: the primary selection (SDL_clipboard.h) -----
//
// The app has one clipboard.  The primary selection (the X11 "middle click"
// text) is kept inside the VM.

static std::string sPrimarySelection;
static std::mutex sPrimarySelectionMx;
static int stub_SDL_SetPrimarySelectionText(const char* text) {
    std::lock_guard<std::mutex> lk(sPrimarySelectionMx);
    sPrimarySelection = text ? text : "";
    return 0;
}
static char* stub_SDL_GetPrimarySelectionText() {
    std::lock_guard<std::mutex> lk(sPrimarySelectionMx);
    return sdlAllocString(sPrimarySelection.c_str());
}
static int stub_SDL_HasPrimarySelectionText() {
    std::lock_guard<std::mutex> lk(sPrimarySelectionMx);
    return sPrimarySelection.empty() ? 0 : 1;
}
