// Connects the game's IL2CPP registration tables to the native libil2cpp.
//
// libil2cpp calls g_CodegenRegistration during startup. In a normal build that
// is generated code; here the tables come from the game's arm64 image, loaded
// as data and rebased, so their pointers are valid host addresses.
#include <cstdio>
#include <cstdlib>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "macho_image.h"
#include "recomp_bridge.h"
#include "recomp_ctx.h"
#include "recomp_eh.h"

struct Il2CppCodeRegistration;
struct Il2CppMetadataRegistration;
struct Il2CppCodeGenOptions;

// Must match libil2cpp/codegen/il2cpp-codegen-il2cpp.h exactly (MSVC mangles
// top-level const on pointer parameters).
void il2cpp_codegen_register(const Il2CppCodeRegistration* const codeRegistration,
                             const Il2CppMetadataRegistration* const metadataRegistration,
                             const Il2CppCodeGenOptions* const codeGenOptions);

namespace recomp
{
    void RegisterRuntimeImports();
    uint64_t ResolveImageSymbol(const MachOImage& image, const std::string& symbol);
    void StartMainThreadSampler();
    void InstallDebugMenu(uint64_t imageBase);
}

namespace
{
    // Offsets in the macOS v1.4.2 arm64 GameAssembly, taken from its own
    // registration function (s_Il2CppCodegenRegistration at 0x125314).
    constexpr uint64_t kCodeRegistration = 0x3b029b0;
    constexpr uint64_t kMetadataRegistration = 0x3b03528;
#ifndef GOSR_BUILD_TAG
#define GOSR_BUILD_TAG "dev"
#endif

    constexpr uint64_t kCodeGenOptions = 0x3558c10;

    recomp::MachOImage* g_image = nullptr;

    // The arm64 image sits next to GameAssembly.dll as GameAssembly_arm64.dylib,
    // unless GOSR_GAME_IMAGE points elsewhere.
    std::wstring ImagePath()
    {
        wchar_t env[MAX_PATH * 4];
        DWORD n = GetEnvironmentVariableW(L"GOSR_GAME_IMAGE", env, DWORD(std::size(env)));
        if (n > 0 && n < std::size(env))
            return env;

        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ImagePath), &self);
        wchar_t path[MAX_PATH * 4];
        DWORD len = GetModuleFileNameW(self, path, DWORD(std::size(path)));
        std::wstring dir(path, len);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        // the thinned slice (development) or the macOS app's universal library
        std::wstring thin = dir + L"GameAssembly_arm64.dylib";
        if (GetFileAttributesW(thin.c_str()) != INVALID_FILE_ATTRIBUTES)
            return thin;
        return dir + L"GoS_Data\\GameAssembly.dylib";
    }

    void RegisterGameCodegen()
    {
        recomp::StartMainThreadSampler();
        g_image = new recomp::MachOImage;
        std::wstring path = ImagePath();
        if (!g_image->Load(path))
        {
            fprintf(stderr, "GoSRecomp: cannot load the game image: %s\n", g_image->GetError().c_str());
            abort();
        }
        recomp::g_imageBase = g_image->Base();
        recomp::g_imageSize = g_image->Size();
        auto unbound = g_image->ApplyBindings([](const std::string& symbol) { return recomp::ResolveImageSymbol(*g_image, symbol); });
        fprintf(stderr, "GoSRecomp " GOSR_BUILD_TAG ": game image at %p, %zu bindings left unbound\n", reinterpret_cast<void*>(g_image->Base()), unbound.size());
        if (GetEnvironmentVariableW(L"GOSR_VERBOSE", nullptr, 0))
        {
            for (const auto& b : unbound)
                fprintf(stderr, "  unbound %s at 0x%llx\n", b.symbol.c_str(), static_cast<unsigned long long>(b.vmAddress));
            fflush(stderr);
        }

        // Point the registration tables at native entry thunks for the
        // recompiled code before libil2cpp reads them.
        recomp::InstallBridges();
        // Diagnostic build: legacy debug menu and its progress-changing hooks are excluded.
        recomp::RegisterRuntimeImports();
        recomp::RegisterExceptionImports();

        uint64_t base = g_image->Base();
        il2cpp_codegen_register(reinterpret_cast<const Il2CppCodeRegistration*>(base + kCodeRegistration),
                                reinterpret_cast<const Il2CppMetadataRegistration*>(base + kMetadataRegistration),
                                reinterpret_cast<const Il2CppCodeGenOptions*>(base + kCodeGenOptions));
    }
}

void (*g_CodegenRegistration)() = RegisterGameCodegen;
