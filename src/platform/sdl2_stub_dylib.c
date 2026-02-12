/*
 * sdl2_stub_dylib.c - Minimal SDL2 stub dynamic library
 *
 * This creates a loadable libSDL2.dylib so Pharo's FFIMacLibraryFinder
 * can find it via dlopen(). The actual SDL2 function implementations
 * are in FFI.cpp's stub functions, which are found via the FFI cache
 * in primitiveLoadSymbolFromModule (checked before dlsym).
 *
 * This dylib just needs to be loadable - the function implementations
 * here are never called because the FFI cache takes priority.
 */

#include <stdint.h>
#include <stddef.h>

// Core
int SDL_Init(uint32_t flags) { return 0; }
void SDL_Quit(void) {}
void SDL_GetVersion(void* ver) {}
const char* SDL_GetError(void) { return "stub"; }
uint32_t SDL_WasInit(uint32_t flags) { return flags; }
int SDL_InitSubSystem(uint32_t flags) { return 0; }
void SDL_QuitSubSystem(uint32_t flags) {}

// Video
int SDL_VideoInit(const char* d) { return 0; }
void SDL_VideoQuit(void) {}

// Window
void* SDL_CreateWindow(const char* t, int x, int y, int w, int h, uint32_t f) { return (void*)1; }
void SDL_DestroyWindow(void* w) {}
void SDL_GetWindowSize(void* w, int* pw, int* ph) { if(pw)*pw=1024; if(ph)*ph=768; }
void SDL_SetWindowSize(void* w, int nw, int nh) {}
void SDL_SetWindowTitle(void* w, const char* t) {}
void SDL_ShowWindow(void* w) {}
void SDL_HideWindow(void* w) {}
void SDL_RaiseWindow(void* w) {}
uint32_t SDL_GetWindowID(void* w) { return 1; }
void* SDL_GetWindowFromID(uint32_t id) { return (void*)1; }
int SDL_SetWindowFullscreen(void* w, uint32_t f) { return 0; }
void SDL_GetWindowPosition(void* w, int* x, int* y) { if(x)*x=0; if(y)*y=0; }
void SDL_SetWindowPosition(void* w, int x, int y) {}
void SDL_SetWindowIcon(void* w, void* i) {}

// Renderer
void* SDL_CreateRenderer(void* w, int i, uint32_t f) { return (void*)2; }
void SDL_DestroyRenderer(void* r) {}
int SDL_RenderClear(void* r) { return 0; }
void SDL_RenderPresent(void* r) {}
int SDL_SetRenderDrawColor(void* r, uint8_t red, uint8_t g, uint8_t b, uint8_t a) { return 0; }

// Texture
void* SDL_CreateTexture(void* r, uint32_t fmt, int acc, int w, int h) { return (void*)3; }
void SDL_DestroyTexture(void* t) {}
int SDL_LockTexture(void* t, void* rect, void** px, int* pitch) { return -1; }
void SDL_UnlockTexture(void* t) {}
int SDL_RenderCopy(void* r, void* t, void* s, void* d) { return 0; }
int SDL_UpdateTexture(void* t, void* r, void* p, int pitch) { return 0; }

// Events
int SDL_PollEvent(void* e) { return 0; }
int SDL_WaitEvent(void* e) { return 0; }
int SDL_PushEvent(void* e) { return 0; }

// Clipboard
char* SDL_GetClipboardText(void) { return (char*)""; }
int SDL_SetClipboardText(const char* t) { return 0; }
int SDL_HasClipboardText(void) { return 0; }

// Cursor
void* SDL_CreateSystemCursor(int id) { return (void*)4; }
void SDL_SetCursor(void* c) {}
void SDL_FreeCursor(void* c) {}
int SDL_ShowCursor(int t) { return 0; }

// Mouse
uint32_t SDL_GetMouseState(int* x, int* y) { if(x)*x=0; if(y)*y=0; return 0; }
uint32_t SDL_GetGlobalMouseState(int* x, int* y) { if(x)*x=0; if(y)*y=0; return 0; }

// Display
int SDL_GetNumVideoDisplays(void) { return 1; }
int SDL_GetDisplayBounds(int d, void* r) { return 0; }

// Timer
uint32_t SDL_GetTicks(void) { return 0; }
uint64_t SDL_GetPerformanceCounter(void) { return 0; }
uint64_t SDL_GetPerformanceFrequency(void) { return 1000000000ULL; }

// Hints
int SDL_SetHint(const char* n, const char* v) { return 1; }

// GL
int SDL_GL_SetAttribute(int a, int v) { return 0; }
void* SDL_GL_CreateContext(void* w) { return (void*)5; }
void SDL_GL_DeleteContext(void* c) {}
int SDL_GL_MakeCurrent(void* w, void* c) { return 0; }
void SDL_GL_SwapWindow(void* w) {}
