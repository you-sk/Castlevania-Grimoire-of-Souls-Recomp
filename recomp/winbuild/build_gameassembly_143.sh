#!/usr/bin/env bash
# Builds GameAssembly.dll for Windows x64 from Linux with clang-cl and lld-link:
# Unity's libil2cpp + Boehm GC + zlib, the recompiled game code, the bridges
# and the runtime glue in this directory.
#
# Required environment:
#   UNITY_IL2CPP   Unity 2020.3.49f1 Editor/Data/il2cpp directory
#   MSVC_SYSROOT   CRT + Windows SDK as produced by `xwin splat`
#   CASE_ALIASES   directory with header case aliases (fix_case.py)
#   UNITY_WIN64    directory with baselib.dll.lib from the Windows IL2CPP support
#   RECOMP_OUT     output of tools/recomp/arm64_recomp.py (registration-derived functions)
#   GAP_OUT        output of arm64_recomp.py --extra-table over gap_functions.py's table
#   BRIDGES_OUT    output of tools/recomp/gen_bridges.py
#   BUILD_DIR      where objects and the DLL are written
set -euo pipefail
ulimit -s unlimited  # the largest recompiled functions need a deep compiler stack

HERE="$(cd "$(dirname "$0")" && pwd)"
RUNTIME="$HERE/../runtime"
JOBS="${JOBS:-4}"
L="$UNITY_IL2CPP"
M="$MSVC_SYSROOT"

SYS="--target=x86_64-pc-windows-msvc /nologo /c /EHsc /MT /GS- -Wno-everything \
  /imsvc$CASE_ALIASES /imsvc$M/crt/include /imsvc$M/sdk/include/ucrt /imsvc$M/sdk/include/um \
  /imsvc$M/sdk/include/shared /imsvc$M/sdk/include/winrt \
  /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00"

IL2CPP_FLAGS="$SYS /O2 /std:c++14 -mcx16 /FI$HERE/msvc_compat.h /DNDEBUG /DGC_NOT_DLL /DRUNTIME_IL2CPP \
  /DHAVE_BDWGC_GC /DBASELIB_INLINE_NAMESPACE=il2cpp_baselib /DUNICODE /D_UNICODE \
  /I$L/libil2cpp /I$L/libil2cpp/pch /I$L/external/bdwgc/include /I$L/external/baselib/Include \
  /I$L/external/baselib/Platforms/Windows/Include /I$L/external/xxHash /I$L/external/zlib \
  /I$L/external/mono /I$L/external/google"

GC_FLAGS="$SYS /O2 /I$L/external/bdwgc/include /I$L/external/bdwgc/libatomic_ops/src /DNDEBUG /DGC_NOT_DLL \
  /DALL_INTERIOR_POINTERS=1 /DGC_GCJ_SUPPORT=1 /DJAVA_FINALIZATION=1 /DNO_EXECUTE_PERMISSION=1 \
  /DGC_NO_THREADS_DISCOVERY=1 /DIGNORE_DYNAMIC_LOADING=1 /DGC_DONT_REGISTER_MAIN_STATIC_DATA=1 \
  /DGC_VERSION_MAJOR=7 /DGC_VERSION_MINOR=7 /DGC_VERSION_MICRO=0 /DGC_THREADS=1 /DUSE_MMAP=1 \
  /DUSE_MUNMAP=1 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS"

GEN_FLAGS="$SYS ${GAME_OPT:-/Od} /std:c++17 /I$RUNTIME /I$RECOMP_OUT /I$BRIDGES_OUT"
GAP_FLAGS="$SYS /O1 /std:c++17 /I$RUNTIME /I$GAP_OUT"
GLUE_FLAGS="$SYS /O2 /std:c++17 /I$RUNTIME"
# handlers calling into libil2cpp need its headers (and C++17 for the adapter)
IMPORT_FLAGS="${IL2CPP_FLAGS/\/std:c++14//std:c++17} /I$RUNTIME /I$HERE /I$L/libil2cpp/icalls"

mkdir -p "$BUILD_DIR"/{il2cpp,gc,zlib,gen,gap,glue,server}
compile() {  # flags, output dir, sources...
    local flags="$1" out="$2"; shift 2
    printf '%s\n' "$@" | xargs -P "$JOBS" -I{} sh -c \
        'o="$0/$(echo {} | tr "/" "_" | sed "s/\.[a-z]*$/.obj/")"; clang-cl-18 '"$flags"' {} /Fo"$o" >"$o.log" 2>&1 || { rm -f "$o"; echo "FAIL {}"; exit 255; }' "$out"
}

echo "== libil2cpp"
mapfile -t IL2CPP_SOURCES < <(find "$L/libil2cpp" -name '*.cpp' | grep -vE '/os/(Posix|WinRT|Android|Emscripten|Lumin|WindowsGames|OSX)/|/debugger/|/metadata/FieldLayout\.cpp$')
compile "$IL2CPP_FLAGS" "$BUILD_DIR/il2cpp" "${IL2CPP_SOURCES[@]}" "$HERE/field_layout_clang_abi.cpp"
compile "$IMPORT_FLAGS" "$BUILD_DIR/glue" "$HERE/runtime_imports.cpp" "$HERE/icall_marshal.cpp" "$HERE/icall_imports.cpp" "$HERE/libc_imports.cpp" "$HERE/mouse_keys.cpp" "$HERE/display.cpp" "$HERE/controller_hooks.cpp" "$HERE/input_render_hooks_143.cpp"

echo "== offline server"
compile "$IMPORT_FLAGS /I$HERE/../server" "$BUILD_DIR/server" "$HERE/../server/local_server.cpp" "$HERE/../server/msgpack.cpp"

echo "== Boehm GC and zlib"
compile "$GC_FLAGS" "$BUILD_DIR/gc" "$L/external/bdwgc/extra/gc.c"
compile "$GC_FLAGS /I$L/external/zlib" "$BUILD_DIR/zlib" "$L"/external/zlib/*.c

echo "== runtime glue"
compile "$GLUE_FLAGS" "$BUILD_DIR/glue" "$RUNTIME/macho_image.cpp" "$RUNTIME/recomp_runtime.cpp" \
    "$RUNTIME/recomp_bridge.cpp" "$RUNTIME/recomp_eh.cpp" "$HERE/codegen_registration.cpp" "$HERE/crash_report.cpp" "$HERE/image_symbols.cpp"

echo "== recompiled game code and bridges"
compile "$GEN_FLAGS" "$BUILD_DIR/gen" "$RECOMP_OUT"/*.cpp "$BRIDGES_OUT"/*.cpp
compile "$GAP_FLAGS" "$BUILD_DIR/gap" "$GAP_OUT"/*.cpp

echo "== link"
lld-link-18 /nologo /dll /out:"$BUILD_DIR/GameAssembly.dll" /map:"$BUILD_DIR/GameAssembly.map" /machine:x64 \
    /libpath:"$M/crt/lib/x86_64" /libpath:"$M/sdk/lib/um/x86_64" /libpath:"$M/sdk/lib/ucrt/x86_64" \
    "$BUILD_DIR"/il2cpp/*.obj "$BUILD_DIR"/gc/*.obj "$BUILD_DIR"/zlib/*.obj "$BUILD_DIR"/glue/*.obj "$BUILD_DIR"/server/*.obj "$BUILD_DIR"/gen/*.obj "$BUILD_DIR"/gap/*.obj \
    "$UNITY_WIN64/baselib.dll.lib" libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib user32.lib advapi32.lib \
    ole32.lib oleaut32.lib shell32.lib ws2_32.lib iphlpapi.lib bcrypt.lib crypt32.lib dbghelp.lib
echo "built $BUILD_DIR/GameAssembly.dll"
