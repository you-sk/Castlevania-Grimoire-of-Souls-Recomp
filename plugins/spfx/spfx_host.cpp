// SPFXUnityPlugin.dll: the macOS SPFX effects plugin, recompiled from its
// arm64 code (tools/recomp/arm64_recomp.py), with a host that loads the
// original plugin image as data, binds its imports and stands in for the
// Unity interfaces it uses. The plugin is shown an OpenGL Core device, and
// its OpenGL calls run on Unity's Direct3D 11 device (spfx_gl.cpp).
#include "spfx_host.h"

#include <atomic>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "macho_image.h"
#include "recomp_bridge.h"
#include "unity_interfaces.h"

namespace
{
    void SpfxAbortStack143(int)
    {
        fprintf(stderr, "SPFX abort stack:\n");
        void* frames[48];
        USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
        {
            HMODULE module = nullptr;
            char path[MAX_PATH] = {};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCSTR>(frames[i]), &module))
                GetModuleFileNameA(module, path, MAX_PATH);
            const char* name = strrchr(path, '\\');
            fprintf(stderr, "  %s+0x%llx\n", name ? name + 1 : path,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module)));
        }
        fflush(stderr);
    }
}

namespace spfx
{
    struct DefinedSymbol { const char* name; uint64_t address; };
    extern const DefinedSymbol kDefinedSymbols[];
    extern const size_t kDefinedSymbolCount;
    extern const uint64_t kModInitFunc;
    extern const size_t kModInitCount;

    uint64_t GuestSymbol(const char* name)
    {
        for (size_t i = 0; i < kDefinedSymbolCount; i++)
            if (!strcmp(kDefinedSymbols[i].name, name))
                return recomp::g_imageBase + kDefinedSymbols[i].address;
        Log("plugin symbol %s missing", name);
        abort();
    }
}

namespace
{
    using recomp::Ctx;
    using recomp::Function;

    recomp::MachOImage* g_image = nullptr;
    std::once_flag g_loadOnce;
    uint64_t g_nextSynthetic = 0;  // guest addresses past the image for host callables

    std::unordered_map<std::string, Function>& Handlers()
    {
        static std::unordered_map<std::string, Function> handlers;
        return handlers;
    }

    std::wstring ModuleDir()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
        wchar_t path[MAX_PATH * 4];
        DWORD len = GetModuleFileNameW(self, path, DWORD(std::size(path)));
        std::wstring dir(path, len);
        return dir.substr(0, dir.find_last_of(L"\\/") + 1);
    }

    // The macOS plugin binary (universal or arm64) next to this DLL, or
    // GOSR_SPFX_IMAGE.
    std::wstring ImagePath()
    {
        wchar_t env[MAX_PATH * 4];
        DWORD n = GetEnvironmentVariableW(L"GOSR_SPFX_IMAGE", env, DWORD(std::size(env)));
        if (n > 0 && n < std::size(env))
            return env;
        return ModuleDir() + L"SPFXUnityPlugin.macho";
    }

    uint64_t Placeholder(const std::string& symbol)
    {
        static std::unordered_map<std::string, std::unique_ptr<uint64_t[]>> blocks;
        auto& block = blocks[symbol];
        if (!block)
            block = std::make_unique<uint64_t[]>(64);
        return reinterpret_cast<uint64_t>(block.get());
    }

    uint64_t g_stackGuard = 0x00ab1c2d3e4f5a00ull;

    uint64_t Resolve(const std::string& symbol)
    {
        static std::unordered_map<std::string, uint64_t> defined = [] {
            std::unordered_map<std::string, uint64_t> m;
            for (size_t i = 0; i < spfx::kDefinedSymbolCount; i++)
                m[spfx::kDefinedSymbols[i].name] = spfx::kDefinedSymbols[i].address;
            return m;
        }();
        uint64_t base = g_image->Base();
        if (auto it = defined.find(symbol); it != defined.end())
            return base + it->second;
        if (symbol == "___stack_chk_guard")
            return reinterpret_cast<uint64_t>(&g_stackGuard);
        auto h = Handlers().find(symbol);
        if (h != Handlers().end())
        {
            if (uint64_t stub = g_image->StubAddress(symbol))
                return base + stub;
            return spfx::GuestCallable(h->second);
        }
        return Placeholder(symbol);
    }

    void Load()
    {
        spfx::RegisterLibcImports();
        spfx::RegisterCxxImports();
        spfx::RegisterGlImports();
        g_image = new recomp::MachOImage;
        std::wstring path = ImagePath();
        if (!g_image->Load(path))
        {
            spfx::Log("cannot load the plugin image %ls: %s (effects are off; run Setup again)", path.c_str(),
                      g_image->GetError().c_str());
            delete g_image;
            g_image = nullptr;
            return;
        }
        recomp::g_imageBase = g_image->Base();
        recomp::g_imageSize = g_image->Size();
        g_nextSynthetic = (g_image->Size() + 0xFFFF) & ~uint64_t(0xFFFF);
        // stubs first, so bound pointers to the same functions reuse them
        for (const auto& [symbol, handler] : Handlers())
            if (uint64_t stub = g_image->StubAddress(symbol))
                recomp::RegisterImport(stub, handler);
        auto unbound = g_image->ApplyBindings(Resolve);
        spfx::Log("plugin image at %p (%llu bytes), %zu bindings unbound", reinterpret_cast<void*>(g_image->Base()),
                  static_cast<unsigned long long>(g_image->Size()), unbound.size());
        // static initializers (pointers rebased to host addresses by the loader)
        const auto* init = reinterpret_cast<const uint64_t*>(g_image->Base() + spfx::kModInitFunc);
        for (size_t i = 0; i < spfx::kModInitCount; i++)
            spfx::CallGuest(init[i]);
    }

    // ------------------------------------------------ Unity interface stand-ins

    IUnityInterfaces* g_realInterfaces = nullptr;
    uint64_t g_guestDeviceEvent = 0;

    struct FakeInterfaces
    {
        uint64_t getInterface, registerInterface, getInterfaceSplit, registerInterfaceSplit;
    } g_fakeInterfaces;

    struct FakeGraphics
    {
        uint64_t getRenderer, registerDeviceEventCallback, unregisterDeviceEventCallback, reserveEventIDRange;
    } g_fakeGraphics;

    bool IsGraphicsGuid(uint64_t high, uint64_t low)
    {
        return high == 0x7CBA0A9CA4DDB544ull && low == 0x8C5AD4926EB17B11ull;
    }

    // IUnityInterfaces::GetInterface(UnityInterfaceGUID): the guest passes the GUID's address
    void GetInterface(Ctx& c)
    {
        const uint64_t* guid = reinterpret_cast<const uint64_t*>(c.x[0]);
        c.x[0] = IsGraphicsGuid(guid[0], guid[1]) ? reinterpret_cast<uint64_t>(&g_fakeGraphics) : 0;
        if (!c.x[0])
            spfx::Log("plugin asked for interface %016llx-%016llx", static_cast<unsigned long long>(guid[0]),
                      static_cast<unsigned long long>(guid[1]));
    }

    void GetInterfaceSplit(Ctx& c)
    {
        c.x[0] = IsGraphicsGuid(c.x[0], c.x[1]) ? reinterpret_cast<uint64_t>(&g_fakeGraphics) : 0;
    }

    void RegisterInterface(Ctx&) {}

    void GetRenderer(Ctx& c) { c.x[0] = kUnityGfxRendererOpenGLCore; }

    void RegisterDeviceEventCallback(Ctx& c) { g_guestDeviceEvent = c.x[0]; }
    void UnregisterDeviceEventCallback(Ctx& c)
    {
        if (g_guestDeviceEvent == c.x[0])
            g_guestDeviceEvent = 0;
    }
    void ReserveEventIDRange(Ctx& c) { c.x[0] = 0; }

    uint64_t g_guestRenderEvent = 0;

    void UNITY_INTERFACE_API NativeRenderEvent(int eventId)
    {
        if (!g_guestRenderEvent)
            return;
        spfx::gl::BeginCameraDraw(eventId);  // the game's event id is the camera id
        spfx::CallGuest(g_guestRenderEvent, uint64_t(int64_t(eventId)));
        spfx::gl::EndCameraDraw();
    }

    void UNITY_INTERFACE_API NativeDeviceEvent(UnityGfxDeviceEventType type)
    {
        // the plugin's own GL objects die with the device
        if (type == kUnityGfxDeviceEventShutdown && g_guestDeviceEvent)
            spfx::CallGuest(g_guestDeviceEvent, uint64_t(type));
    }
}

namespace spfx
{
    bool EnsureLoaded()
    {
        std::call_once(g_loadOnce, Load);
        return g_image != nullptr;
    }

    void* FallbackHandle()
    {
        static std::atomic<uintptr_t> next{ 0x10000 };
        return reinterpret_cast<void*>(next.fetch_add(0x10));
    }

    void RegisterHandler(const std::string& symbol, Function handler)
    {
        Handlers()[symbol] = handler;
    }

    uint64_t GuestCallable(Function handler)
    {
        uint64_t guest = g_nextSynthetic;
        g_nextSynthetic += 0x10;
        recomp::RegisterImport(guest, handler);
        return g_image->Base() + guest;
    }

    uint64_t CallGuest(uint64_t hostAddress, uint64_t a0, uint64_t a1, uint64_t a2)
    {
        Function f = recomp::FindFunction(hostAddress - g_image->Base());
        if (!f)
        {
            Log("call to %p, not a plugin function", reinterpret_cast<void*>(hostAddress));
            return 0;
        }
        recomp::GuestStackScope scope;
        Ctx c;
        memset(&c, 0, sizeof(c));
        c.sp = scope.Top();
        c.x[0] = a0;
        c.x[1] = a1;
        c.x[2] = a2;
        f(c);
        return c.x[0];
    }

    void Log(const char* format, ...)
    {
        static std::mutex lock;
        static FILE* f = [] {
            freopen("gosr_spfx_stderr.log", "w", stderr);
            setvbuf(stderr, nullptr, _IONBF, 0);
            std::signal(SIGABRT, SpfxAbortStack143);
            FILE* file = fopen("gosr_spfx.log", "w");
            return file ? file : stderr;
        }();
        std::lock_guard<std::mutex> guard(lock);
        // seconds since the plugin loaded, to see how long things take
        static ULONGLONG start = GetTickCount64();
        fprintf(f, "[%7.2f] ", double(GetTickCount64() - start) / 1000.0);
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fputc('\n', f);
        fflush(f);
    }

    IUnityInterfaces* RealInterfaces() { return g_realInterfaces; }


    // Opt-in, bounded diagnostics for the confirmed coin resources only.
    struct CoinTrace143 { unsigned remaining; int status; };
    std::mutex g_coinLock143;
    std::unordered_map<void*, CoinTrace143> g_coins143;
    bool CoinTraceEnabled143()
    {
        static bool enabled = GetEnvironmentVariableW(L"GOSR_SPFX_TRACE_COINS", nullptr, 0) != 0;
        return enabled;
    }
    void NoteCoinCreated143(void* handle, const char* file, int32_t bytes, int32_t id, void* const* textures, int32_t count)
    {
        if (!CoinTraceEnabled143() || !file || (!strstr(file, "eff_obj_2502") && !strstr(file, "eff_obj_2503"))) return;
        std::lock_guard<std::mutex> lock(g_coinLock143);
        if (g_coins143.size() >= 1024) return;
        g_coins143[handle] = {64, -999};
        if ((strstr(file, "eff_obj_2502") || strstr(file, "eff_obj_2503")) && textures && count > 0)
            gl::MarkCoinTexture143(uint32_t(reinterpret_cast<uintptr_t>(textures[0])));
        Log("COIN create %s handle %p bytes %d id %d textures %d", file, handle, bytes, id, count);
        for (int32_t i = 0; textures && i < count && i < 16; ++i)
            Log("COIN texture[%d] GL-name %llu", i, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(textures[i])));
    }
    bool TraceCoinHandle143(void* handle)
    {
        if (!CoinTraceEnabled143()) return false;
        std::lock_guard<std::mutex> lock(g_coinLock143);
        auto it = g_coins143.find(handle);
        return it != g_coins143.end() && it->second.remaining && it->second.remaining--;
    }
    void NoteCoinStatus143(void* handle, int32_t status)
    {
        if (!CoinTraceEnabled143()) return;
        std::lock_guard<std::mutex> lock(g_coinLock143);
        auto it = g_coins143.find(handle);
        if (it != g_coins143.end() && it->second.status != status)
        {
            Log("COIN status handle %p: %d -> %d", handle, it->second.status, status);
            it->second.status = status;
        }
    }

    // Effects whose status takes over half a second to change after creation
    // (they are loading): logged with the time it took, the first 300.
    struct PendingInstance
    {
        ULONGLONG created;
        int32_t status;
        std::string file;
    };
    std::mutex g_pendingLock;
    std::unordered_map<void*, PendingInstance> g_pending;
    int g_slowLogged = 0;

    void NoteInstanceCreated(void* handle, const char* file)
    {
        std::lock_guard<std::mutex> guard(g_pendingLock);
        if (g_pending.size() < 4096 && handle)
            g_pending[handle] = { GetTickCount64(), -1, file ? file : "?" };
    }

    void NoteInstanceStatus(void* handle, int32_t status)
    {
        NoteCoinStatus143(handle, status);
        std::lock_guard<std::mutex> guard(g_pendingLock);
        auto it = g_pending.find(handle);
        if (it == g_pending.end())
            return;
        if (it->second.status == -1)
            it->second.status = status;
        if (status == it->second.status)
            return;
        ULONGLONG ms = GetTickCount64() - it->second.created;
        if (ms >= 500 && g_slowLogged++ < 300)
            Log("effect %s: status %d -> %d after %llu ms", it->second.file.c_str(), it->second.status, status, ms);
        g_pending.erase(it);
    }

    bool TraceCalls()
    {
        static const bool on = GetEnvironmentVariableW(L"GOSR_SPFX_TRACE", nullptr, 0) != 0;
        return on;
    }
}

#define EXPORT extern "C" __declspec(dllexport)

EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces)
{
    g_realInterfaces = interfaces;
    if (!spfx::EnsureLoaded())
        return;
    g_fakeInterfaces = { spfx::GuestCallable(GetInterface), spfx::GuestCallable(RegisterInterface),
                         spfx::GuestCallable(GetInterfaceSplit), spfx::GuestCallable(RegisterInterface) };
    g_fakeGraphics = { spfx::GuestCallable(GetRenderer), spfx::GuestCallable(RegisterDeviceEventCallback),
                       spfx::GuestCallable(UnregisterDeviceEventCallback), spfx::GuestCallable(ReserveEventIDRange) };
    if (auto* graphics = interfaces->Get<IUnityGraphics>())
        graphics->RegisterDeviceEventCallback(NativeDeviceEvent);
    spfx::Log("UnityPluginLoad");
    spfx::CallGuest(spfx::GuestSymbol("_UnityPluginLoad"), reinterpret_cast<uint64_t>(&g_fakeInterfaces));
}

EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
{
    if (g_image)
        spfx::CallGuest(spfx::GuestSymbol("_UnityPluginUnload"));
}

EXPORT void* SPFX_PLUGIN_GetRenderingCallback()
{
    if (!spfx::EnsureLoaded())
        return reinterpret_cast<void*>(&NativeRenderEvent);
    g_guestRenderEvent = spfx::CallGuest(spfx::GuestSymbol("_SPFX_PLUGIN_GetRenderingCallback"));
    return reinterpret_cast<void*>(&NativeRenderEvent);
}
