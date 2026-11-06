#include "save_root_143.h"
// Display settings from gosr.ini:
//
//   [display]
//   fps=        ; frame rate cap: 30, 60, 120, 144, ... 0 = no cap. Empty: the game's own (60)
//   vsync=      ; 1 = wait for the monitor's refresh, 0 = don't. Empty: the game's own
//   mode=       ; fullscreen, borderless or windowed. Empty: as Unity remembers it
//   width=      ; window or screen size; empty: the monitor's
//   height=
//
// The game sets its frame rate and vsync itself (bs.Awake, and vsync again
// whenever it thinks the refresh rate changed); those engine calls are
// wrapped so the configured values win. With vsync on, the monitor's refresh
// rate is the cap: fps then only lowers it to a fraction of the refresh rate.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "il2cpp-api.h"

namespace
{
    struct Settings
    {
        int fps = -2;    // -2 game's own, 0 no cap
        int vsync = -1;  // -1 game's own
        int mode = -1;   // UnityEngine.FullScreenMode, -1 untouched
        int width = 0, height = 0;
    };

    std::wstring IniPath()
    {
        wchar_t path[MAX_PATH * 4];
        DWORD len = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
        std::wstring p(path, len);
        return p.substr(0, p.find_last_of(L"\\/") + 1) + L"gosr.ini";
    }

    std::wstring ReadKey(const std::wstring& ini, const wchar_t* key)
    {
        wchar_t value[64] = {};
        GetPrivateProfileStringW(L"display", key, L"", value, DWORD(std::size(value)), ini.c_str());
        std::wstring v(value);
        while (!v.empty() && iswspace(v.back()))
            v.pop_back();
        for (auto& c : v)
            c = wchar_t(towlower(c));
        return v;
    }

    const Settings& Config()
    {
        static const Settings s = [] {
            Settings r;
            std::wstring ini = IniPath();
            std::wstring v = ReadKey(ini, L"fps");
            if (!v.empty() && iswdigit(v[0]))
                r.fps = _wtoi(v.c_str());
            v = ReadKey(ini, L"vsync");
            if (!v.empty())
                r.vsync = (v == L"1" || v == L"on" || v == L"true" || v == L"yes") ? 1 : 0;
            v = ReadKey(ini, L"mode");
            if (v == L"fullscreen" || v == L"exclusive")
                r.mode = 0;  // ExclusiveFullScreen
            else if (v == L"borderless")
                r.mode = 1;  // FullScreenWindow
            else if (v == L"windowed" || v == L"window")
                r.mode = 3;  // Windowed
            r.width = _wtoi(ReadKey(ini, L"width").c_str());
            r.height = _wtoi(ReadKey(ini, L"height").c_str());
            if (r.fps != -2 || r.vsync != -1 || r.mode != -1)
                fprintf(stderr, "GoSRecomp: display fps %d vsync %d mode %d size %dx%d\n", r.fps, r.vsync, r.mode, r.width, r.height);
            return r;
        }();
        return s;
    }

    int RefreshRate()
    {
        DEVMODEW dm = { sizeof(dm) };
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            return int(dm.dmDisplayFrequency);
        return 60;
    }

    using IntSetter = void (*)(int32_t);
    IntSetter g_setTargetFrameRate, g_setVSyncCount;

    int VSyncCount()
    {
        const Settings& s = Config();
        if (s.vsync == 0)
            return 0;
        if (s.fps <= 0)
            return 1;
        int count = RefreshRate() / s.fps;  // e.g. 144 Hz with fps=72: every 2nd refresh
        return count < 1 ? 1 : (count > 4 ? 4 : count);
    }

    void SetTargetFrameRate(int32_t fps)
    {
        const Settings& s = Config();
        if (s.fps != -2 && fps == 60)  // the game's normal rate; it asks 30 on purpose elsewhere
            fps = s.fps == 0 ? -1 : s.fps;
        g_setTargetFrameRate(fps);
    }

    void SetVSyncCount(int32_t count)
    {
        const Settings& s = Config();
        if (s.vsync != -1 || s.fps != -2)
            count = s.vsync == 1 ? VSyncCount() : 0;  // a set fps without vsync=1: no vsync, so the cap applies
        g_setVSyncCount(count);
    }
}

namespace recomp
{
    void* WrapDisplayIcall(const char* name, void* original)
    {
        if (void* isolated = gosr143::WrapPersistentPath(name, original); isolated != original) return isolated;
        if (!original)
            return original;
        if (!strncmp(name, "UnityEngine.Application::set_targetFrameRate", 44))
            return g_setTargetFrameRate = reinterpret_cast<IntSetter>(original), reinterpret_cast<void*>(&SetTargetFrameRate);
        if (!strncmp(name, "UnityEngine.QualitySettings::set_vSyncCount", 43))
            return g_setVSyncCount = reinterpret_cast<IntSetter>(original), reinterpret_cast<void*>(&SetVSyncCount);
        return original;
    }

    // First frame: the window mode and size (the frame settings follow the
    // game's own calls).
    void ApplyDisplaySettings()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        const Settings& s = Config();
        if (s.mode == -1 && !s.width)
            return;
        using SetResolution = void (*)(int32_t, int32_t, int32_t, int32_t);
        using GetMode = int32_t (*)();
        auto set = reinterpret_cast<SetResolution>(il2cpp_resolve_icall("UnityEngine.Screen::SetResolution"));
        auto getMode = reinterpret_cast<GetMode>(il2cpp_resolve_icall("UnityEngine.Screen::get_fullScreenMode"));
        if (!set)
        {
            fprintf(stderr, "GoSRecomp: Screen.SetResolution not available\n");
            return;
        }
        int mode = s.mode != -1 ? s.mode : (getMode ? getMode() : 1);
        int width = s.width, height = s.height;
        if (width <= 0 || height <= 0)
        {
            width = GetSystemMetrics(SM_CXSCREEN);
            height = GetSystemMetrics(SM_CYSCREEN);
            if (mode == 3)  // a window a bit smaller than the screen
                width = width * 3 / 4, height = height * 3 / 4;
        }
        fprintf(stderr, "GoSRecomp: display %dx%d mode %d\n", width, height, mode);
        set(width, height, mode, 0);
    }
}
