// SDL2Register.inc - registration of the stubs of SDL2StubsCore.inc and of the
// C library functions that SDL2 exports under its own names.
//
// Included by FFI.cpp inside registerSDL2FullStubs().

    // Error text
    SDL_REG(SetError); SDL_REG(ClearError); SDL_REG(Error); SDL_REG(GetErrorMsg);

    // Events
    SDL_REG(PumpEvents); SDL_REG(PeepEvents); SDL_REG(HasEvents); SDL_REG(HasEvent);
    SDL_REG(FlushEvents); SDL_REG(FlushEvent); SDL_REG(SetEventFilter); SDL_REG(GetEventFilter);
    SDL_REG(FilterEvents); SDL_REG(AddEventWatch); SDL_REG(DelEventWatch); SDL_REG(RegisterEvents);
    SDL_REG(WaitEventTimeout);

    // Threads, locks, atomics, timers
    SDL_REG(CreateThread); SDL_REG(CreateThreadWithStackSize); SDL_REG(GetThreadName); SDL_REG(ThreadID);
    SDL_REG(GetThreadID); SDL_REG(SetThreadPriority); SDL_REG(WaitThread); SDL_REG(DetachThread);
    SDL_REG(TLSCreate); SDL_REG(TLSGet); SDL_REG(TLSSet); SDL_REG(TLSCleanup);
    SDL_REG(CreateMutex); SDL_REG(DestroyMutex); SDL_REG(LockMutex); SDL_REG(TryLockMutex); SDL_REG(UnlockMutex);
    SDL_REG(CreateSemaphore); SDL_REG(DestroySemaphore); SDL_REG(SemWait); SDL_REG(SemTryWait);
    SDL_REG(SemWaitTimeout); SDL_REG(SemPost); SDL_REG(SemValue);
    SDL_REG(CreateCond); SDL_REG(DestroyCond); SDL_REG(CondSignal); SDL_REG(CondBroadcast);
    SDL_REG(CondWait); SDL_REG(CondWaitTimeout);
    SDL_REG(AtomicAdd); SDL_REG(AtomicCAS); SDL_REG(AtomicSet); SDL_REG(AtomicGet); SDL_REG(AtomicCASPtr);
    SDL_REG(AtomicSetPtr); SDL_REG(AtomicGetPtr); SDL_REG(AtomicTryLock); SDL_REG(AtomicLock);
    SDL_REG(AtomicUnlock); SDL_REG(MemoryBarrierReleaseFunction); SDL_REG(MemoryBarrierAcquireFunction);
    SDL_REG(AddTimer); SDL_REG(RemoveTimer);

    // Hints
    SDL_REG(SetHintWithPriority); SDL_REG(GetHintBoolean); SDL_REG(ResetHint); SDL_REG(ResetHints);
    SDL_REG(ClearHints); SDL_REG(AddHintCallback); SDL_REG(DelHintCallback);

    // Log and assertions
    SDL_REG(LogSetAllPriority); SDL_REG(LogSetPriority); SDL_REG(LogGetPriority); SDL_REG(LogResetPriorities);
    SDL_REG(LogGetOutputFunction); SDL_REG(LogSetOutputFunction); SDL_REG(LogMessageV); SDL_REG(LogVerbose);
    SDL_REG(LogDebug); SDL_REG(LogInfo); SDL_REG(LogWarn); SDL_REG(LogError); SDL_REG(LogCritical);
    SDL_REG(LogMessage); SDL_REG(Log);
    SDL_REG(ReportAssertion); SDL_REG(SetAssertionHandler); SDL_REG(GetAssertionHandler);
    SDL_REG(GetDefaultAssertionHandler); SDL_REG(GetAssertionReport); SDL_REG(ResetAssertionReport);

    // Environment, text and memory helpers
    SDL_REG(getenv); SDL_REG(setenv); SDL_REG(itoa); SDL_REG(uitoa); SDL_REG(ltoa); SDL_REG(ultoa);
    SDL_REG(lltoa); SDL_REG(ulltoa); SDL_REG(strrev); SDL_REG(strupr); SDL_REG(strlwr); SDL_REG(strtokr);
    SDL_REG(strlcat); SDL_REG(strlcpy); SDL_REG(strcasestr); SDL_REG(utf8strlen); SDL_REG(utf8strnlen);
    SDL_REG(utf8strlcpy); SDL_REG(crc16); SDL_REG(crc32); SDL_REG(abs);
    SDL_REG(isalpha); SDL_REG(isalnum); SDL_REG(isblank); SDL_REG(iscntrl); SDL_REG(isdigit); SDL_REG(isxdigit);
    SDL_REG(ispunct); SDL_REG(isspace); SDL_REG(isupper); SDL_REG(islower); SDL_REG(isprint); SDL_REG(isgraph);
    SDL_REG(toupper); SDL_REG(tolower);
    SDL_REG(SIMDAlloc); SDL_REG(SIMDRealloc); SDL_REG(SIMDFree); SDL_REG(SIMDGetAlignment);

    // The C library under the SDL names.  The signatures and the results are those of the C library.
    {
        struct Direct { const char* name; void* fn; };
        const Direct direct[] = {
            { "SDL_strlen", reinterpret_cast<void*>(+[](const char* s) { return strlen(s); }) },
            { "SDL_strcmp", reinterpret_cast<void*>(static_cast<int (*)(const char*, const char*)>(strcmp)) },
            { "SDL_strncmp", reinterpret_cast<void*>(static_cast<int (*)(const char*, const char*, size_t)>(strncmp)) },
            { "SDL_strcasecmp", reinterpret_cast<void*>(static_cast<int (*)(const char*, const char*)>(strcasecmp)) },
            { "SDL_strncasecmp", reinterpret_cast<void*>(static_cast<int (*)(const char*, const char*, size_t)>(strncasecmp)) },
            { "SDL_strstr", reinterpret_cast<void*>(+[](const char* h, const char* n) { return const_cast<char*>(strstr(h, n)); }) },
            { "SDL_strchr", reinterpret_cast<void*>(+[](const char* s, int c) { return const_cast<char*>(strchr(s, c)); }) },
            { "SDL_strrchr", reinterpret_cast<void*>(+[](const char* s, int c) { return const_cast<char*>(strrchr(s, c)); }) },
            { "SDL_strtol", reinterpret_cast<void*>(static_cast<long (*)(const char*, char**, int)>(strtol)) },
            { "SDL_strtoul", reinterpret_cast<void*>(static_cast<unsigned long (*)(const char*, char**, int)>(strtoul)) },
            { "SDL_strtoll", reinterpret_cast<void*>(static_cast<long long (*)(const char*, char**, int)>(strtoll)) },
            { "SDL_strtoull", reinterpret_cast<void*>(static_cast<unsigned long long (*)(const char*, char**, int)>(strtoull)) },
            { "SDL_strtod", reinterpret_cast<void*>(static_cast<double (*)(const char*, char**)>(strtod)) },
            { "SDL_atoi", reinterpret_cast<void*>(+[](const char* s) { return atoi(s); }) },
            { "SDL_atof", reinterpret_cast<void*>(+[](const char* s) { return atof(s); }) },
            { "SDL_memset", reinterpret_cast<void*>(static_cast<void* (*)(void*, int, size_t)>(memset)) },
            { "SDL_memmove", reinterpret_cast<void*>(static_cast<void* (*)(void*, const void*, size_t)>(memmove)) },
            { "SDL_memcmp", reinterpret_cast<void*>(static_cast<int (*)(const void*, const void*, size_t)>(memcmp)) },
            { "SDL_qsort", reinterpret_cast<void*>(+[](void* b, size_t n, size_t s, int (*c)(const void*, const void*)) { qsort(b, n, s, c); }) },
            { "SDL_bsearch", reinterpret_cast<void*>(+[](const void* k, const void* b, size_t n, size_t s, int (*c)(const void*, const void*)) { return bsearch(k, b, n, s, c); }) },
            { "SDL_snprintf", reinterpret_cast<void*>(reinterpret_cast<int (*)(char*, size_t, const char*, ...)>(snprintf)) },
            { "SDL_vsnprintf", reinterpret_cast<void*>(reinterpret_cast<int (*)(char*, size_t, const char*, va_list)>(vsnprintf)) },
            { "SDL_asprintf", reinterpret_cast<void*>(reinterpret_cast<int (*)(char**, const char*, ...)>(asprintf)) },
            { "SDL_vasprintf", reinterpret_cast<void*>(reinterpret_cast<int (*)(char**, const char*, va_list)>(vasprintf)) },
            { "SDL_sscanf", reinterpret_cast<void*>(reinterpret_cast<int (*)(const char*, const char*, ...)>(sscanf)) },
            { "SDL_vsscanf", reinterpret_cast<void*>(reinterpret_cast<int (*)(const char*, const char*, va_list)>(vsscanf)) },
            { "SDL_wcslen", reinterpret_cast<void*>(static_cast<size_t (*)(const wchar_t*)>(wcslen)) },
            { "SDL_wcscmp", reinterpret_cast<void*>(static_cast<int (*)(const wchar_t*, const wchar_t*)>(wcscmp)) },
            { "SDL_wcsncmp", reinterpret_cast<void*>(static_cast<int (*)(const wchar_t*, const wchar_t*, size_t)>(wcsncmp)) },
            { "SDL_wcsstr", reinterpret_cast<void*>(+[](const wchar_t* h, const wchar_t* n) { return const_cast<wchar_t*>(wcsstr(h, n)); }) },
            { "SDL_wcsdup", reinterpret_cast<void*>(static_cast<wchar_t* (*)(const wchar_t*)>(wcsdup)) },
            { "SDL_wcslcpy", reinterpret_cast<void*>(+[](wchar_t* d, const wchar_t* s, size_t n) -> size_t {
                const size_t len = wcslen(s);
                if (n > 0) { const size_t c = len < n - 1 ? len : n - 1; wmemcpy(d, s, c); d[c] = L'\0'; }
                return len; }) },
            { "SDL_wcslcat", reinterpret_cast<void*>(+[](wchar_t* d, const wchar_t* s, size_t n) -> size_t {
                size_t dl = 0; while (dl < n && d[dl]) dl++;
                const size_t sl = wcslen(s);
                if (dl == n) return n + sl;
                const size_t room = n - dl - 1, c = sl < room ? sl : room;
                wmemcpy(d + dl, s, c); d[dl + c] = L'\0';
                return dl + sl; }) },
            { "SDL_wcscasecmp", reinterpret_cast<void*>(+[](const wchar_t* a, const wchar_t* b) -> int {
                for (;; ++a, ++b) { const wint_t x = towlower(static_cast<wint_t>(*a)), y = towlower(static_cast<wint_t>(*b));
                    if (x != y) return x < y ? -1 : 1; if (!*a) return 0; } }) },
            { "SDL_wcsncasecmp", reinterpret_cast<void*>(+[](const wchar_t* a, const wchar_t* b, size_t n) -> int {
                for (; n; --n, ++a, ++b) { const wint_t x = towlower(static_cast<wint_t>(*a)), y = towlower(static_cast<wint_t>(*b));
                    if (x != y) return x < y ? -1 : 1; if (!*a) return 0; } return 0; }) },
            { "SDL_strdup", reinterpret_cast<void*>(+[](const char* s) { return sdlAllocString(s); }) },
            // Math: the double and the float forms of the C library.
            { "SDL_acos", reinterpret_cast<void*>(static_cast<double (*)(double)>(acos)) },
            { "SDL_acosf", reinterpret_cast<void*>(static_cast<float (*)(float)>(acosf)) },
            { "SDL_asin", reinterpret_cast<void*>(static_cast<double (*)(double)>(asin)) },
            { "SDL_asinf", reinterpret_cast<void*>(static_cast<float (*)(float)>(asinf)) },
            { "SDL_atan", reinterpret_cast<void*>(static_cast<double (*)(double)>(atan)) },
            { "SDL_atanf", reinterpret_cast<void*>(static_cast<float (*)(float)>(atanf)) },
            { "SDL_atan2", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(atan2)) },
            { "SDL_atan2f", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(atan2f)) },
            { "SDL_ceil", reinterpret_cast<void*>(static_cast<double (*)(double)>(ceil)) },
            { "SDL_ceilf", reinterpret_cast<void*>(static_cast<float (*)(float)>(ceilf)) },
            { "SDL_copysign", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(copysign)) },
            { "SDL_copysignf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(copysignf)) },
            { "SDL_cos", reinterpret_cast<void*>(static_cast<double (*)(double)>(cos)) },
            { "SDL_cosf", reinterpret_cast<void*>(static_cast<float (*)(float)>(cosf)) },
            { "SDL_exp", reinterpret_cast<void*>(static_cast<double (*)(double)>(exp)) },
            { "SDL_expf", reinterpret_cast<void*>(static_cast<float (*)(float)>(expf)) },
            { "SDL_fabs", reinterpret_cast<void*>(static_cast<double (*)(double)>(fabs)) },
            { "SDL_fabsf", reinterpret_cast<void*>(static_cast<float (*)(float)>(fabsf)) },
            { "SDL_floor", reinterpret_cast<void*>(static_cast<double (*)(double)>(floor)) },
            { "SDL_floorf", reinterpret_cast<void*>(static_cast<float (*)(float)>(floorf)) },
            { "SDL_trunc", reinterpret_cast<void*>(static_cast<double (*)(double)>(trunc)) },
            { "SDL_truncf", reinterpret_cast<void*>(static_cast<float (*)(float)>(truncf)) },
            { "SDL_fmod", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(fmod)) },
            { "SDL_fmodf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(fmodf)) },
            { "SDL_log", reinterpret_cast<void*>(static_cast<double (*)(double)>(log)) },
            { "SDL_logf", reinterpret_cast<void*>(static_cast<float (*)(float)>(logf)) },
            { "SDL_log10", reinterpret_cast<void*>(static_cast<double (*)(double)>(log10)) },
            { "SDL_log10f", reinterpret_cast<void*>(static_cast<float (*)(float)>(log10f)) },
            { "SDL_pow", reinterpret_cast<void*>(static_cast<double (*)(double, double)>(pow)) },
            { "SDL_powf", reinterpret_cast<void*>(static_cast<float (*)(float, float)>(powf)) },
            { "SDL_round", reinterpret_cast<void*>(static_cast<double (*)(double)>(round)) },
            { "SDL_roundf", reinterpret_cast<void*>(static_cast<float (*)(float)>(roundf)) },
            { "SDL_lround", reinterpret_cast<void*>(static_cast<long (*)(double)>(lround)) },
            { "SDL_lroundf", reinterpret_cast<void*>(static_cast<long (*)(float)>(lroundf)) },
            { "SDL_scalbn", reinterpret_cast<void*>(static_cast<double (*)(double, int)>(scalbn)) },
            { "SDL_scalbnf", reinterpret_cast<void*>(static_cast<float (*)(float, int)>(scalbnf)) },
            { "SDL_sin", reinterpret_cast<void*>(static_cast<double (*)(double)>(sin)) },
            { "SDL_sinf", reinterpret_cast<void*>(static_cast<float (*)(float)>(sinf)) },
            { "SDL_sqrt", reinterpret_cast<void*>(static_cast<double (*)(double)>(sqrt)) },
            { "SDL_sqrtf", reinterpret_cast<void*>(static_cast<float (*)(float)>(sqrtf)) },
            { "SDL_tan", reinterpret_cast<void*>(static_cast<double (*)(double)>(tan)) },
            { "SDL_tanf", reinterpret_cast<void*>(static_cast<float (*)(float)>(tanf)) },
        };
        for (const Direct& d : direct) registerFunction(d.name, d.fn);
    }

    // Memory
    registerFunction("SDL_malloc", reinterpret_cast<void*>(+[](size_t n) { return sdlMemory().malloc_(n); }));
    registerFunction("SDL_calloc", reinterpret_cast<void*>(+[](size_t n, size_t s) { return sdlMemory().calloc_(n, s); }));
    registerFunction("SDL_realloc", reinterpret_cast<void*>(+[](void* p, size_t n) { return sdlMemory().realloc_(p, n); }));
    SDL_REG(SetMemoryFunctions); SDL_REG(GetMemoryFunctions); SDL_REG(GetOriginalMemoryFunctions);
    SDL_REG(GetNumAllocations);

    // Platform, CPU, power, version, shared objects, paths, GUID
    SDL_REG(GetPlatform); SDL_REG(GetCPUCount); SDL_REG(GetCPUCacheLineSize); SDL_REG(GetSystemRAM);
    SDL_REG(HasRDTSC); SDL_REG(HasMMX); SDL_REG(HasSSE); SDL_REG(HasSSE2); SDL_REG(HasSSE3); SDL_REG(HasSSE41);
    SDL_REG(HasSSE42); SDL_REG(HasAVX); SDL_REG(HasAVX2); SDL_REG(HasAVX512F); SDL_REG(HasNEON);
    SDL_REG(Has3DNow); SDL_REG(HasAltiVec); SDL_REG(HasARMSIMD); SDL_REG(HasLSX); SDL_REG(HasLASX);
    SDL_REG(GetPowerInfo); SDL_REG(GetRevision); SDL_REG(GetRevisionNumber); SDL_REG(SetMainReady);
    SDL_REG(LoadObject); SDL_REG(LoadFunction); SDL_REG(UnloadObject); SDL_REG(GetBasePath); SDL_REG(GetPrefPath);
    SDL_REG(GUIDToString); SDL_REG(GUIDFromString);

    // RWops
    SDL_REG(AllocRW); SDL_REG(FreeRW); SDL_REG(RWFromFile); SDL_REG(RWFromFP); SDL_REG(RWFromMem);
    SDL_REG(RWFromConstMem); SDL_REG(RWsize); SDL_REG(RWseek); SDL_REG(RWtell); SDL_REG(RWread);
    SDL_REG(RWwrite); SDL_REG(RWclose); SDL_REG(LoadFile_RW); SDL_REG(LoadFile);
    SDL_REG(ReadU8); SDL_REG(ReadLE16); SDL_REG(ReadBE16); SDL_REG(ReadLE32); SDL_REG(ReadBE32);
    SDL_REG(ReadLE64); SDL_REG(ReadBE64); SDL_REG(WriteU8); SDL_REG(WriteLE16); SDL_REG(WriteBE16);
    SDL_REG(WriteLE32); SDL_REG(WriteBE32); SDL_REG(WriteLE64); SDL_REG(WriteBE64);

    // Rectangles
    SDL_REG(HasIntersection); SDL_REG(HasIntersectionF); SDL_REG(IntersectRect); SDL_REG(IntersectFRect);
    SDL_REG(UnionRect); SDL_REG(UnionFRect); SDL_REG(EnclosePoints); SDL_REG(EncloseFPoints);
    SDL_REG(IntersectRectAndLine); SDL_REG(IntersectFRectAndLine);

    // Devices: the generated stubs, then the hand written ones
    registerSDL2DeviceStubs();
    SDL_REG(NumJoysticks); SDL_REG(NumHaptics); SDL_REG(NumSensors); SDL_REG(JoystickUpdate);
    SDL_REG(GameControllerUpdate); SDL_REG(SensorUpdate); SDL_REG(LockJoysticks); SDL_REG(UnlockJoysticks);
    SDL_REG(LockSensors); SDL_REG(UnlockSensors); SDL_REG(JoystickEventState); SDL_REG(GameControllerEventState);
    SDL_REG(JoystickGetGUIDString); SDL_REG(JoystickGetGUIDFromString); SDL_REG(GetJoystickGUIDInfo);
    SDL_REG(GameControllerGetStringForAxis); SDL_REG(GameControllerGetStringForButton);
    SDL_REG(GameControllerGetAxisFromString); SDL_REG(GameControllerGetButtonFromString);
    SDL_REG(GameControllerAddMapping); SDL_REG(GameControllerNumMappings); SDL_REG(GameControllerMappingForIndex);
    SDL_REG(GameControllerMappingForGUID); SDL_REG(GameControllerAddMappingsFromRW);
    SDL_REG(GetNumTouchDevices); SDL_REG(GetTouchDevice); SDL_REG(GetTouchName); SDL_REG(GetTouchDeviceType);
    SDL_REG(GetNumTouchFingers); SDL_REG(GetTouchFinger);
    SDL_REG(hid_init); SDL_REG(hid_exit); SDL_REG(hid_device_change_count); SDL_REG(hid_free_enumeration);
    SDL_REG(hid_ble_scan);
    SDL_REG(GetNumAudioDrivers); SDL_REG(GetAudioDriver); SDL_REG(GetCurrentAudioDriver); SDL_REG(AudioInit);
    SDL_REG(AudioQuit); SDL_REG(GetNumAudioDevices); SDL_REG(GetAudioDeviceName); SDL_REG(OpenAudioDevice);
    SDL_REG(OpenAudio);

    // Window, display, keyboard, mouse, clipboard, and the features the host lacks
    SDL_REG(GetWindowMinimumSize); SDL_REG(GetWindowMaximumSize); SDL_REG(SetWindowMinimumSize);
    SDL_REG(SetWindowMaximumSize); SDL_REG(GetWindowSizeInPixels); SDL_REG(SetWindowData); SDL_REG(GetWindowData);
    SDL_REG(SetWindowGrab); SDL_REG(SetWindowMouseGrab); SDL_REG(SetWindowKeyboardGrab); SDL_REG(GetWindowGrab);
    SDL_REG(GetWindowMouseGrab); SDL_REG(GetWindowKeyboardGrab); SDL_REG(GetGrabbedWindow);
    SDL_REG(SetWindowMouseRect); SDL_REG(GetWindowMouseRect);
    SDL_REG(GetWindowBordersSize); SDL_REG(SetWindowOpacity); SDL_REG(GetWindowOpacity); SDL_REG(SetWindowModalFor);
    SDL_REG(FlashWindow); SDL_REG(SetWindowAlwaysOnTop); SDL_REG(GetWindowICCProfile); SDL_REG(CreateWindowFrom);
    SDL_REG(GetWindowBrightness); SDL_REG(SetWindowBrightness); SDL_REG(SetWindowGammaRamp); SDL_REG(GetWindowGammaRamp);
    SDL_REG(SetWindowDisplayMode); SDL_REG(GetDesktopDisplayMode); SDL_REG(GetClosestDisplayMode);
    SDL_REG(GetDisplayName); SDL_REG(GetDisplayOrientation); SDL_REG(GetWindowPixelFormat);
    SDL_REG(GetPointDisplayIndex); SDL_REG(GetRectDisplayIndex); SDL_REG(GetNumVideoDrivers); SDL_REG(GetVideoDriver);
    SDL_REG(EnableScreenSaver); SDL_REG(IsScreenSaverEnabled);
    SDL_REG(GL_LoadLibrary); SDL_REG(GL_UnloadLibrary); SDL_REG(GL_ExtensionSupported); SDL_REG(GL_ResetAttributes);
    SDL_REG(GL_GetCurrentContext); SDL_REG(GL_GetCurrentWindow); SDL_REG(GL_GetSwapInterval); SDL_REG(GL_SetSwapInterval);
    SDL_REG(Vulkan_LoadLibrary); SDL_REG(Vulkan_UnloadLibrary); SDL_REG(Vulkan_GetVkGetInstanceProcAddr);
    SDL_REG(Vulkan_GetInstanceExtensions); SDL_REG(Vulkan_CreateSurface); SDL_REG(Vulkan_GetDrawableSize);
    SDL_REG(Metal_CreateView); SDL_REG(Metal_DestroyView); SDL_REG(Metal_GetLayer); SDL_REG(Metal_GetDrawableSize);
    SDL_REG(CreateShapedWindow); SDL_REG(IsShapedWindow); SDL_REG(SetWindowShape); SDL_REG(GetShapedWindowMode);
    SDL_REG(ShowSimpleMessageBox); SDL_REG(ShowMessageBox);
    SDL_REG(GetScancodeName); SDL_REG(GetScancodeFromName); SDL_REG(GetKeyFromScancode); SDL_REG(GetScancodeFromKey);
    SDL_REG(GetKeyName); SDL_REG(GetKeyFromName); SDL_REG(GetKeyboardState); SDL_REG(GetKeyboardFocus);
    SDL_REG(ResetKeyboard); SDL_REG(ClearComposition); SDL_REG(HasScreenKeyboardSupport);
    SDL_REG(IsScreenKeyboardShown); SDL_REG(IsTextInputShown);
    SDL_REG(GetCursor); SDL_REG(GetDefaultCursor); SDL_REG(GetMouseFocus); SDL_REG(GetRelativeMouseMode);
    SDL_REG(GetRelativeMouseState); SDL_REG(CaptureMouse); SDL_REG(WarpMouseGlobal); SDL_REG(WarpMouseInWindow);
    SDL_REG(SetPrimarySelectionText); SDL_REG(GetPrimarySelectionText); SDL_REG(HasPrimarySelectionText);

    // Pixel formats, surfaces, render and texture getters
    sdl2::surfaceFreeHook() = sdl2::surf::forget;
    SDL_REG(GetPixelFormatName); SDL_REG(PixelFormatEnumToMasks); SDL_REG(MasksToPixelFormatEnum); SDL_REG(AllocFormat);
    SDL_REG(FreeFormat); SDL_REG(AllocPalette); SDL_REG(FreePalette); SDL_REG(SetPaletteColors);
    SDL_REG(SetPixelFormatPalette); SDL_REG(MapRGB); SDL_REG(MapRGBA); SDL_REG(GetRGB); SDL_REG(GetRGBA);
    SDL_REG(CalculateGammaRamp);
    SDL_REG(CreateRGBSurfaceWithFormatFrom); SDL_REG(ConvertSurface); SDL_REG(DuplicateSurface); SDL_REG(FillRect);
    SDL_REG(FillRects); SDL_REG(SetClipRect); SDL_REG(GetClipRect); SDL_REG(SetColorKey); SDL_REG(GetColorKey);
    SDL_REG(HasColorKey); SDL_REG(SetSurfaceColorMod); SDL_REG(GetSurfaceColorMod); SDL_REG(SetSurfaceAlphaMod);
    SDL_REG(GetSurfaceAlphaMod); SDL_REG(SetSurfaceBlendMode); SDL_REG(GetSurfaceBlendMode); SDL_REG(SetSurfaceRLE);
    SDL_REG(HasSurfaceRLE); SDL_REG(SetSurfacePalette); SDL_REG(UpperBlit); SDL_REG(LowerBlit);
    SDL_REG(UpperBlitScaled); SDL_REG(LowerBlitScaled); SDL_REG(SoftStretch); SDL_REG(SoftStretchLinear);
    SDL_REG(PremultiplyAlpha); SDL_REG(ConvertPixels); SDL_REG(LoadBMP_RW); SDL_REG(SaveBMP_RW);
    SDL_REG(SetYUVConversionMode); SDL_REG(GetYUVConversionMode); SDL_REG(GetYUVConversionModeForResolution);
    SDL_REG(GetRenderDrawColor); SDL_REG(GetRenderDrawBlendMode); SDL_REG(GetRenderTarget); SDL_REG(RenderGetWindow);
    SDL_REG(GetRenderer); SDL_REG(RenderFlush); SDL_REG(RenderGetMetalLayer); SDL_REG(RenderGetMetalCommandEncoder);
    SDL_REG(GetTextureColorMod); SDL_REG(GetTextureAlphaMod); SDL_REG(GetTextureBlendMode); SDL_REG(GetTextureScaleMode);
    SDL_REG(SetTextureUserData); SDL_REG(GetTextureUserData); SDL_REG(UpdateYUVTexture); SDL_REG(UpdateNVTexture);
    SDL_REG(GL_BindTexture); SDL_REG(GL_UnbindTexture); SDL_REG(CreateTextureFromSurface);
    SDL_REG(CreateSoftwareRenderer); SDL_REG(CreateWindowAndRenderer); SDL_REG(LockTextureToSurface);

    // Audio conversion
    SDL_REG(NewAudioStream); SDL_REG(AudioStreamPut); SDL_REG(AudioStreamGet); SDL_REG(AudioStreamAvailable);
    SDL_REG(AudioStreamFlush); SDL_REG(AudioStreamClear); SDL_REG(FreeAudioStream); SDL_REG(BuildAudioCVT);
    SDL_REG(ConvertAudio); SDL_REG(MixAudioFormat); SDL_REG(MixAudio); SDL_REG(LoadWAV_RW); SDL_REG(FreeWAV);

    // Text conversion, locale, URLs
    SDL_REG(iconv_open); SDL_REG(iconv_close); SDL_REG(iconv); SDL_REG(iconv_string);
    SDL_REG(GetPreferredLocales); SDL_REG(OpenURL);
    SDL_REG(OnApplicationWillTerminate); SDL_REG(OnApplicationDidReceiveMemoryWarning); SDL_REG(OnApplicationWillResignActive);
    SDL_REG(OnApplicationDidEnterBackground); SDL_REG(OnApplicationWillEnterForeground); SDL_REG(OnApplicationDidBecomeActive);
    SDL_REG(IsTablet);
