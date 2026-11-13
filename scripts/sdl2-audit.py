#!/usr/bin/env python3
"""Audit the SDL2 stub layer against the full SDL2 function list.

Usage:
  sdl2-audit.py <repo root>           print the functions without a stub
  sdl2-audit.py <repo root> --table   print the markdown table of every function

Reads scripts/sdl2-prototypes.json (every function of the SDL 2.26.5 headers),
the registration lines of src/vm/FFI.cpp and the SDL2*.inc files, and the
differential scripts in scripts/cogdiff/.
"""
import glob
import json
import re
import sys
from collections import defaultdict

root = sys.argv[1]
protos = json.load(open(root + "/scripts/sdl2-prototypes.json"))
text = ""
for name in ("FFI.cpp", "SDL2Register.inc", "SDL2StubsDevices.inc"):
    text += open(root + "/src/vm/" + name).read()
have = set(re.findall(r'registerFunction\("(SDL_\w+)"', text))
have |= set(re.findall(r'\{ "(SDL_\w+)", reinterpret_cast', text))
have |= {"SDL_" + m for m in re.findall(r"SDL_REG\((\w+)\)", text)}

# Functions that the video driver of the app does not have.  Each answers the
# error that SDL gives on a driver without the feature.
HOST_LACKS = set("""
SDL_GL_LoadLibrary SDL_GL_UnloadLibrary SDL_GL_ExtensionSupported SDL_GL_GetCurrentContext SDL_GL_GetCurrentWindow
SDL_GL_GetSwapInterval SDL_GL_SetSwapInterval SDL_GL_CreateContext SDL_GL_DeleteContext SDL_GL_MakeCurrent
SDL_GL_SwapWindow SDL_GL_BindTexture SDL_GL_UnbindTexture
SDL_Vulkan_LoadLibrary SDL_Vulkan_UnloadLibrary SDL_Vulkan_GetVkGetInstanceProcAddr SDL_Vulkan_GetInstanceExtensions
SDL_Vulkan_CreateSurface SDL_Vulkan_GetDrawableSize SDL_Metal_CreateView SDL_Metal_DestroyView SDL_Metal_GetLayer
SDL_Metal_GetDrawableSize SDL_RenderGetMetalLayer SDL_RenderGetMetalCommandEncoder
SDL_CreateShapedWindow SDL_IsShapedWindow SDL_SetWindowShape SDL_GetShapedWindowMode SDL_GetWindowWMInfo
SDL_GetWindowBordersSize SDL_SetWindowOpacity SDL_SetWindowModalFor SDL_FlashWindow SDL_GetWindowICCProfile
SDL_CreateWindowFrom SDL_SetWindowBrightness SDL_SetWindowGammaRamp SDL_CaptureMouse SDL_WarpMouseGlobal
SDL_SetWindowHitTest SDL_ShowSimpleMessageBox SDL_ShowMessageBox SDL_OpenURL SDL_UpdateYUVTexture SDL_UpdateNVTexture
""".split())
NO_DEVICE_HEADERS = {"SDL_joystick.h", "SDL_gamecontroller.h", "SDL_haptic.h", "SDL_sensor.h", "SDL_touch.h",
                     "SDL_hidapi.h", "SDL_gesture.h"}
NO_DEVICE_AUDIO = set("""
SDL_GetNumAudioDevices SDL_GetAudioDeviceName SDL_GetAudioDeviceSpec SDL_GetDefaultAudioInfo SDL_OpenAudioDevice
SDL_OpenAudio SDL_CloseAudio SDL_CloseAudioDevice SDL_PauseAudio SDL_PauseAudioDevice SDL_GetAudioStatus
SDL_GetAudioDeviceStatus SDL_LockAudio SDL_LockAudioDevice SDL_UnlockAudio SDL_UnlockAudioDevice SDL_QueueAudio
SDL_DequeueAudio SDL_GetQueuedAudioSize SDL_ClearQueuedAudio SDL_MixAudio
""".split())
ABSENT_HEADERS = {"SDL_system.h", "SDL_main.h"}

tested = set()
for path in glob.glob(root + "/scripts/cogdiff/sdl2diff_*.st"):
    tested |= set(re.findall(r"\b(SDL_\w+)\b", open(path).read()))


def status(name, header):
    if name not in have:
        if header in ABSENT_HEADERS:
            return "absent", "platform only: Android, Windows, GDK, WinRT, Linux or the UIKit app delegate; real libSDL2 on macOS does not export it"
        return "MISSING", ""
    if name in HOST_LACKS:
        return "host lacks", "answers the SDL error of a video driver without the feature"
    if header in NO_DEVICE_HEADERS or name in NO_DEVICE_AUDIO:
        return "no device", "answers as SDL does when no device exists"
    return "implemented", ""


if "--table" in sys.argv:
    print("| Function | Header | Status | Compared with real SDL | Note |")
    print("| --- | --- | --- | --- | --- |")
    for name in sorted(protos):
        header = protos[name][0]
        st, note = status(name, header)
        print("| %s | %s | %s | %s | %s |" % (name, header[:-2], st, "yes" if name in tested else "", note))
else:
    missing = defaultdict(list)
    for name, (header, ret, params) in protos.items():
        if status(name, header)[0] in ("MISSING", "absent"):
            missing[header].append(name)
    for header in sorted(missing):
        print(header, len(missing[header]), " ".join(n[4:] for n in sorted(missing[header])))
    print("registered:", len(have & set(protos)), "of", len(protos))
