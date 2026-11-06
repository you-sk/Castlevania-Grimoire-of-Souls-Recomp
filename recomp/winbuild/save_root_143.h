#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "il2cpp-api.h"

namespace gosr143 {
using StringGetter = Il2CppString* (*)();
static StringGetter originalPersistentPath;
static Il2CppString* PersistentPath() {
    const wchar_t* configured = _wgetenv(L"GOSR_SAVE_ROOT");
    if (!configured || !*configured) return originalPersistentPath();
    // The launcher creates this directory and provides a fully qualified path.
    // A malformed override must never fall back to the user's normal saves.
    std::wstring path(configured);
    const bool drive = path.size()>3 && path[1]==L':' && (path[2]==L'\\'||path[2]==L'/');
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (!drive || attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_DIRECTORY)) {
        fputs("GoSRecomp: invalid GOSR_SAVE_ROOT; refusing default save directory\n", stderr);
        abort();
    }
    static bool logged=false;
    if (!logged) { fwprintf(stderr,L"GoSRecomp: isolated persistentDataPath=%ls\n",path.c_str());logged=true; }
    static_assert(sizeof(wchar_t)==sizeof(Il2CppChar));
    return il2cpp_string_new_utf16(reinterpret_cast<const Il2CppChar*>(path.c_str()),static_cast<int32_t>(path.size()));
}
static void* WrapPersistentPath(const char* name,void* original) {
    constexpr char prefix[]="UnityEngine.Application::get_persistentDataPath";
    const size_t n=sizeof(prefix)-1;
    if (original && !strncmp(name,prefix,n) && (name[n]==0 || name[n]=='(')) {
        originalPersistentPath=reinterpret_cast<StringGetter>(original);
        return reinterpret_cast<void*>(&PersistentPath);
    }
    return original;
}
}
