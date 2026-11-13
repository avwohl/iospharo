// SDL2StubsDeviceHand.inc - the device-family functions that do real work or
// that answer by argument: GUID text, controller name and mapping tables,
// event state, touch, and the audio calls that need no device.
//
// The generated stubs (SDL2StubsDevices.inc) cover the calls whose answer is
// the same for every argument while no device exists.
// Included by FFI.cpp inside namespace pharo::ffi.

// ----- Joystick, controller, haptic and sensor counts -----

static int stub_SDL_NumJoysticks() { return 0; }
static int stub_SDL_NumHaptics() { return 0; }
static int stub_SDL_NumSensors() { return 0; }
static void stub_SDL_JoystickUpdate() {}
static void stub_SDL_GameControllerUpdate() {}
static void stub_SDL_SensorUpdate() {}
static void stub_SDL_LockJoysticks() {}
static void stub_SDL_UnlockJoysticks() {}
static void stub_SDL_LockSensors() {}
static void stub_SDL_UnlockSensors() {}

// SDL_JoystickEventState and SDL_GameControllerEventState: SDL_QUERY (-1) answers
// the state, SDL_IGNORE (0) and SDL_ENABLE (1) set it and answer the new state;
// any other value changes nothing and answers itself.
static int sJoystickEventState = 1;
static int sControllerEventState = 1;
static int deviceEventState(int& state, int requested) {
    if (requested == 0 || requested == 1) { state = requested; return state; }
    return requested == -1 ? state : requested;
}
static int stub_SDL_JoystickEventState(int s) { return deviceEventState(sJoystickEventState, s); }
static int stub_SDL_GameControllerEventState(int s) { return deviceEventState(sControllerEventState, s); }

// ----- GUID text and GUID fields -----

static void stub_SDL_JoystickGetGUIDString(sdl2::base::Guid g, char* s, int n) { sdl2::base::guidToString(g, s, n); }
static sdl2::base::Guid stub_SDL_JoystickGetGUIDFromString(const char* s) { return sdl2::base::guidFromString(s); }

// SDL_GetJoystickGUIDInfo reads the vendor, product, version and name CRC out of
// a GUID of the standard form (bus, CRC, vendor, 0, product, 0, version, ...).
static void stub_SDL_GetJoystickGUIDInfo(sdl2::base::Guid g, uint16_t* vendor, uint16_t* product,
                                         uint16_t* version, uint16_t* crc16) {
    constexpr uint16_t kBusVirtual = 255;       // SDL_HARDWARE_BUS_VIRTUAL
    constexpr uint16_t kFirstPrintable = 32;    // a bus value below ' ' is a bus number
    uint16_t w[8];
    memcpy(w, g.data, sizeof(w));               // the words are little endian, as on every supported target
    const uint16_t bus = w[0];
    uint16_t v = 0, p = 0, ver = 0, crc = 0;
    if ((bus < kFirstPrintable || bus == kBusVirtual) && w[3] == 0 && w[5] == 0) {
        v = w[2]; p = w[4]; ver = w[6]; crc = w[1];
    } else if (bus < kFirstPrintable) {
        crc = w[1];
    }
    if (vendor) *vendor = v;
    if (product) *product = p;
    if (version) *version = ver;
    if (crc16) *crc16 = crc;
}

// ----- Game controller names and mappings -----

static const char* const kControllerAxisNames[] = { "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };
static const char* const kControllerButtonNames[] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad" };
constexpr int kControllerAxisCount = 6;
constexpr int kControllerButtonCount = 21;

static const char* stub_SDL_GameControllerGetStringForAxis(int axis) {
    return axis >= 0 && axis < kControllerAxisCount ? kControllerAxisNames[axis] : nullptr;
}
static const char* stub_SDL_GameControllerGetStringForButton(int button) {
    return button >= 0 && button < kControllerButtonCount ? kControllerButtonNames[button] : nullptr;
}
static int stub_SDL_GameControllerGetAxisFromString(const char* s) {
    if (s && (*s == '+' || *s == '-')) ++s;
    if (!s || !*s) return -1;
    for (int i = 0; i < kControllerAxisCount; i++) if (strcasecmp(s, kControllerAxisNames[i]) == 0) return i;
    return -1;
}
static int stub_SDL_GameControllerGetButtonFromString(const char* s) {
    if (!s || !*s) return -1;
    for (int i = 0; i < kControllerButtonCount; i++) if (strcasecmp(s, kControllerButtonNames[i]) == 0) return i;
    return -1;
}

// The mapping table starts empty: no controller database is built in.  Each
// mapping is the text "<32 hex digit GUID>,<name>,<element>:<binding>,...".
struct DeviceMappings {
    std::mutex mx;
    std::vector<std::string> mappings;
};
static DeviceMappings& deviceMappings() {
    static DeviceMappings* m = new DeviceMappings;
    return *m;
}

static std::string mappingGuid(const std::string& m) { return m.substr(0, m.find(',')); }

static int stub_SDL_GameControllerAddMapping(const char* mapping) {
    if (!mapping) return sdl2::sys::invalidParam("mappingString");
    const std::string m = mapping;
    const size_t c1 = m.find(',');
    if (c1 == std::string::npos || c1 == 0) {
        sdl2::sys::setError((std::string("Couldn't parse GUID from ") + m).c_str());
        return -1;
    }
    const size_t c2 = m.find(',', c1 + 1);
    if (c2 == std::string::npos || c2 == c1 + 1) {
        sdl2::sys::setError((std::string("Couldn't parse name from ") + m).c_str());
        return -1;
    }
    DeviceMappings& t = deviceMappings();
    std::lock_guard<std::mutex> lk(t.mx);
    for (std::string& have : t.mappings) {
        if (strcasecmp(mappingGuid(have).c_str(), mappingGuid(m).c_str()) == 0) { have = m; return 0; }
    }
    t.mappings.push_back(m);
    return 1;
}

static int stub_SDL_GameControllerNumMappings() {
    DeviceMappings& t = deviceMappings();
    std::lock_guard<std::mutex> lk(t.mx);
    return static_cast<int>(t.mappings.size());
}

static char* stub_SDL_GameControllerMappingForIndex(int index) {
    DeviceMappings& t = deviceMappings();
    std::lock_guard<std::mutex> lk(t.mx);
    if (index < 0 || index >= static_cast<int>(t.mappings.size())) {
        sdl2::sys::setError("Mapping not available");
        return nullptr;
    }
    return sdlAllocString(t.mappings[static_cast<size_t>(index)].c_str());
}

static char* stub_SDL_GameControllerMappingForGUID(sdl2::base::Guid guid) {
    char text[40];
    sdl2::base::guidToString(guid, text, sizeof(text));
    DeviceMappings& t = deviceMappings();
    std::lock_guard<std::mutex> lk(t.mx);
    for (const std::string& m : t.mappings)
        if (strcasecmp(mappingGuid(m).c_str(), text) == 0) return sdlAllocString(m.c_str());
    sdl2::sys::setError("Mapping not available");
    return nullptr;
}

// Each line of the stream that is not empty and does not start with '#' is a mapping.
static int stub_SDL_GameControllerAddMappingsFromRW(void* rwops, int freerw) {
    if (!rwops) return sdl2::sys::setError("Invalid RWops");
    size_t size = 0;
    char* data = static_cast<char*>(sdl2::rw::loadFileRW(static_cast<sdl2::rw::RWops*>(rwops), &size, freerw));
    if (!data) return sdl2::sys::setError("Could not read DB");
    int added = 0;
    size_t pos = 0;
    while (pos < size) {
        size_t end = pos;
        while (end < size && data[end] != '\n' && data[end] != '\r') ++end;
        const std::string line(data + pos, end - pos);
        if (!line.empty() && line[0] != '#') {
            const int r = stub_SDL_GameControllerAddMapping(line.c_str());
            if (r > 0) ++added;
        }
        pos = end + 1;
    }
    sdlMemory().free_(data);
    return added;
}

// ----- Touch (SDL_touch.h): no touch device exists -----

static int stub_SDL_GetNumTouchDevices() { return 0; }
static int64_t stub_SDL_GetTouchDevice(int index) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Unknown touch device index %d", index);
    sdl2::sys::setError(buf);
    return 0;
}
static const char* stub_SDL_GetTouchName(int index) {
    (void)index;
    sdl2::sys::setError("Unknown touch device");
    return nullptr;
}
static int stub_SDL_GetTouchDeviceType(int64_t id) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Unknown touch device id %d, cannot reset", static_cast<int>(id));
    sdl2::sys::setError(buf);
    return -1;
}
static int stub_SDL_GetNumTouchFingers(int64_t id) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Unknown touch device id %d, cannot reset", static_cast<int>(id));
    sdl2::sys::setError(buf);
    return 0;
}
static void* stub_SDL_GetTouchFinger(int64_t id, int index) {
    (void)index;
    char buf[64];
    snprintf(buf, sizeof(buf), "Unknown touch device id %d, cannot reset", static_cast<int>(id));
    sdl2::sys::setError(buf);
    return nullptr;
}

// ----- HID (SDL_hidapi.h): no HID device exists -----

static int stub_SDL_hid_init() { return 0; }
static int stub_SDL_hid_exit() { return 0; }
static uint32_t stub_SDL_hid_device_change_count() { return 1; }   // the count starts at 1
static void stub_SDL_hid_free_enumeration(void*) {}
static void stub_SDL_hid_ble_scan(int) {}

// ----- Audio (SDL_audio.h) -----
//
// The app has no audio device.  The audio subsystem is a driver named "none"
// with no output and no capture device.  The conversion helpers (audio
// streams, SDL_MixAudioFormat, WAV loading) need no device and work.

static const char kAudioDriverName[] = "none";

static int stub_SDL_GetNumAudioDrivers() { return 1; }
static const char* stub_SDL_GetAudioDriver(int index) { return index == 0 ? kAudioDriverName : nullptr; }
static const char* stub_SDL_GetCurrentAudioDriver() { return kAudioDriverName; }

static int stub_SDL_AudioInit(const char* driver) {
    if (driver && strcmp(driver, kAudioDriverName) != 0) {
        sdl2::sys::setError((std::string("Audio target '") + driver + "' not available").c_str());
        return -1;
    }
    return 0;
}
static void stub_SDL_AudioQuit() {}

static int stub_SDL_GetNumAudioDevices(int isCapture) { (void)isCapture; return 0; }

static const char* stub_SDL_GetAudioDeviceName(int index, int isCapture) {
    (void)index; (void)isCapture;
    sdl2::sys::invalidParam("index");
    return nullptr;
}

// The default device of a driver with no device cannot be opened.
static uint32_t stub_SDL_OpenAudioDevice(const char* device, int isCapture, const void* desired, void* obtained, int allowedChanges) {
    (void)obtained; (void)allowedChanges;
    if (!desired) { sdl2::sys::invalidParam("desired"); return 0; }
    if (isCapture) { sdl2::sys::setError("No capture support"); return 0; }
    if (device) { sdl2::sys::setError("No such device."); return 0; }
    sdl2::sys::setError("No available audio device");
    return 0;
}

static int stub_SDL_OpenAudio(void* desired, void* obtained) {
    if (!desired) return sdl2::sys::invalidParam("desired");
    return sdl2::sys::setError("No available audio device");
}

// ----- Audio streams, converters, mixing, WAV (SDL2Audio.hpp) -----

namespace sdlaudio = sdl2::audio;

static void* stub_SDL_NewAudioStream(uint16_t sf, uint8_t sc, int sr, uint16_t df, uint8_t dc, int dr) {
    return sdlaudio::newStream(sf, sc, sr, df, dc, dr);
}
static int stub_SDL_AudioStreamPut(void* s, const void* buf, int len) { return sdlaudio::streamPut(static_cast<sdlaudio::Stream*>(s), buf, len); }
static int stub_SDL_AudioStreamGet(void* s, void* buf, int len) { return sdlaudio::streamGet(static_cast<sdlaudio::Stream*>(s), buf, len); }
static int stub_SDL_AudioStreamAvailable(void* s) { return sdlaudio::streamAvailable(static_cast<sdlaudio::Stream*>(s)); }
static int stub_SDL_AudioStreamFlush(void* s) { return sdlaudio::streamFlush(static_cast<sdlaudio::Stream*>(s)); }
static void stub_SDL_AudioStreamClear(void* s) { sdlaudio::streamClear(static_cast<sdlaudio::Stream*>(s)); }
static void stub_SDL_FreeAudioStream(void* s) { delete static_cast<sdlaudio::Stream*>(s); }
static int stub_SDL_BuildAudioCVT(void* cvt, uint16_t sf, uint8_t sc, int sr, uint16_t df, uint8_t dc, int dr) {
    return sdlaudio::buildCvt(static_cast<sdlaudio::Cvt*>(cvt), sf, sc, sr, df, dc, dr);
}
static int stub_SDL_ConvertAudio(void* cvt) { return sdlaudio::convertAudio(static_cast<sdlaudio::Cvt*>(cvt)); }
static void stub_SDL_MixAudioFormat(uint8_t* dst, const uint8_t* src, uint16_t format, uint32_t len, int volume) {
    sdlaudio::mixAudioFormat(dst, src, format, len, volume);
}
// The legacy SDL_MixAudio mixes in the format of the open audio device; none is open.
static void stub_SDL_MixAudio(uint8_t* dst, const uint8_t* src, uint32_t len, int volume) {
    (void)dst; (void)src; (void)len; (void)volume;
    sdl2::sys::setError("Invalid audio device ID");
}
static void* stub_SDL_LoadWAV_RW(void* src, int freesrc, void* spec, uint8_t** buf, uint32_t* len) {
    return sdlaudio::loadWav(static_cast<sdl2::rw::RWops*>(src), freesrc, static_cast<sdlaudio::Spec*>(spec), buf, len,
                             [](size_t n) -> void* { return sdlMemory().malloc_(n); }, [](void* p) { sdlMemory().free_(p); });
}
static void stub_SDL_FreeWAV(uint8_t* buf) { sdlMemory().free_(buf); }

// ----- Text conversion (SDL_iconv) -----

static void* stub_SDL_iconv_open(const char* to, const char* from) {
    void* cd = sdl2::iconvlib::open(to, from);
    return cd ? cd : reinterpret_cast<void*>(static_cast<intptr_t>(-1));   // (SDL_iconv_t)-1 means "not supported"
}
static int stub_SDL_iconv_close(void* cd) {
    if (cd && cd != reinterpret_cast<void*>(static_cast<intptr_t>(-1))) delete static_cast<sdl2::iconvlib::Context*>(cd);
    return 0;
}
static size_t stub_SDL_iconv(void* cd, const char** inbuf, size_t* inleft, char** outbuf, size_t* outleft) {
    if (!cd || cd == reinterpret_cast<void*>(static_cast<intptr_t>(-1))) return sdl2::iconvlib::kError;
    return sdl2::iconvlib::convert(static_cast<sdl2::iconvlib::Context*>(cd), inbuf, inleft, outbuf, outleft);
}
static char* stub_SDL_iconv_string(const char* to, const char* from, const char* in, size_t inLeft) {
    return sdl2::iconvlib::convertString(to, from, in, inLeft, [](size_t n) -> void* { return sdlMemory().malloc_(n); },
                                         [](void* p, size_t n) -> void* { return sdlMemory().realloc_(p, n); },
                                         [](void* p) { sdlMemory().free_(p); });
}

// ----- Locale and URLs -----

struct DeviceLocale { const char* language; const char* country; };   // SDL_Locale

// SDL_GetPreferredLocales: one entry per preferred language, then {null, null}, in one block.
static void* stub_SDL_GetPreferredLocales() {
    std::vector<std::pair<std::string, std::string>> found;
#if defined(__APPLE__)
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if (langs) {
        for (CFIndex i = 0; i < CFArrayGetCount(langs); i++) {
            char buf[64] = {};
            CFStringGetCString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(langs, i)), buf, sizeof(buf), kCFStringEncodingUTF8);
            std::string tag = buf;
            for (char& c : tag) if (c == '_') c = '-';
            const size_t dash = tag.find('-');
            std::string language = tag.substr(0, dash), country;
            const size_t last = tag.rfind('-');
            if (last != std::string::npos && last != dash - 0 && tag.size() - last - 1 == 2) country = tag.substr(last + 1);
            else if (dash != std::string::npos && tag.size() - dash - 1 == 2) country = tag.substr(dash + 1);
            found.push_back({ language, country });
        }
        CFRelease(langs);
    }
#else
    for (const char* var : { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" }) {
        const char* value = sdl2::base::envLookup(var);
        if (!value || !*value) continue;
        std::string v = value;
        v = v.substr(0, v.find('.'));
        const size_t us = v.find('_');
        found.push_back({ v.substr(0, us), us == std::string::npos ? std::string() : v.substr(us + 1) });
        break;
    }
#endif
    if (found.empty()) { sdl2::sys::setError("Couldn't find any preferred locales"); return nullptr; }
    size_t strings = 0;
    for (auto& p : found) strings += p.first.size() + 1 + p.second.size() + 1;
    const size_t total = (found.size() + 1) * sizeof(DeviceLocale) + strings;
    auto* block = static_cast<uint8_t*>(sdlMemory().calloc_(1, total));
    if (!block) { sdl2::sys::setError("Out of memory"); return nullptr; }
    auto* array = reinterpret_cast<DeviceLocale*>(block);
    char* text = reinterpret_cast<char*>(block + (found.size() + 1) * sizeof(DeviceLocale));
    for (size_t i = 0; i < found.size(); i++) {
        array[i].language = text;
        memcpy(text, found[i].first.c_str(), found[i].first.size() + 1);
        text += found[i].first.size() + 1;
        if (!found[i].second.empty()) {
            array[i].country = text;
            memcpy(text, found[i].second.c_str(), found[i].second.size() + 1);
            text += found[i].second.size() + 1;
        } else {
            ++text;   // one zero byte, so the strings stay aligned with the size counted above
        }
    }
    return array;
}

// The VM has no call that hands a URL to the host system, so SDL_OpenURL fails
// with SDL's "That operation is not supported".
static int stub_SDL_OpenURL(const char* url) {
    if (!url) return sdl2::sys::invalidParam("url");
    return sdl2::sys::setError(kUnsupported);
}

// ----- Application life cycle events (SDL_system.h) -----
//
// The host calls these when the app changes state.  Each queues the SDL event
// of the same name, through the filter and the watches.

static void sendAppEvent(uint32_t type) {
    sdl2::ev::Event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.timestamp = stub_SDL_GetTicks();
    sdl2::ev::enqueue(e);
}
constexpr uint32_t kAppTerminating = 0x101, kAppLowMemory = 0x102, kAppWillEnterBackground = 0x103,
                   kAppDidEnterBackground = 0x104, kAppWillEnterForeground = 0x105, kAppDidEnterForeground = 0x106;
static void stub_SDL_OnApplicationWillTerminate() { sendAppEvent(kAppTerminating); }
static void stub_SDL_OnApplicationDidReceiveMemoryWarning() { sendAppEvent(kAppLowMemory); }
static void stub_SDL_OnApplicationWillResignActive() { sendAppEvent(kAppWillEnterBackground); }
static void stub_SDL_OnApplicationDidEnterBackground() { sendAppEvent(kAppDidEnterBackground); }
static void stub_SDL_OnApplicationWillEnterForeground() { sendAppEvent(kAppWillEnterForeground); }
static void stub_SDL_OnApplicationDidBecomeActive() { sendAppEvent(kAppDidEnterForeground); }

// The host is a phone, a tablet or a Mac; the VM cannot tell, so the answer is "no tablet".
static int stub_SDL_IsTablet() { return 0; }
