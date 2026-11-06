#!/usr/bin/env bash
# Builds SPFXUnityPlugin.dll from the macOS plugin's arm64 code.
# usage: build.sh <SPFXUnityPlugin Mach-O (universal or arm64)> <out dir>
#   (MSVC_SYSROOT: xwin CRT + SDK; JOBS: parallel compiles)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
IN="${1:?usage: build.sh <plugin binary> <out dir>}"
OUT="${2:?usage: build.sh <plugin binary> <out dir>}"
M="${MSVC_SYSROOT:?set MSVC_SYSROOT to an xwin splat directory (CRT + Windows SDK)}"
CASE="${CASE_ALIASES:?set CASE_ALIASES to the header case-alias directory (recomp/winbuild/fix_case.py)}"
JOBS="${JOBS:-4}"
mkdir -p "$OUT/gen" "$OUT/obj"

# arm64 slice, function table, translation
llvm-lipo-18 "$IN" -thin arm64 -output "$OUT/spfx_arm64" 2>/dev/null || cp "$IN" "$OUT/spfx_arm64"
python3 -I "$ROOT/tools/spfx/function_table.py" "$OUT/spfx_arm64" "$OUT/ft.json"
# translate into a scratch folder; keep unchanged files so they are not recompiled
rm -rf "$OUT/gen_new"
(ulimit -s unlimited; python3 -I "$ROOT/tools/recomp/arm64_recomp_143.py" "$OUT/spfx_arm64" "$OUT/ft.json" "$OUT/gen_new" --split 16 | head -3)
cp "$ROOT/recomp/runtime/recomp_simd.h" "$OUT/gen_new/"
for f in "$OUT"/gen_new/*; do cmp -s "$f" "$OUT/gen/$(basename "$f")" || cp "$f" "$OUT/gen/"; done
rm -rf "$OUT/gen_new"
python3 -I "$HERE/gen_symbols.py" "$OUT/spfx_arm64" "$OUT/spfx_symbols.cpp"
python3 -I "$HERE/gen_exports.py" "$ROOT/tools/spfx/externs.txt" "$OUT/spfx_arm64" "$OUT/spfx_exports.cpp"
for f in spfx_symbols.cpp spfx_exports.cpp; do cmp -s "$OUT/$f" "$OUT/gen/$f" || cp "$OUT/$f" "$OUT/gen/"; done

SYS="--target=x86_64-pc-windows-msvc /nologo /c /EHsc /MT /GS- -Wno-everything /std:c++17 /imsvc$CASE /imsvc$M/crt/include \
  /imsvc$M/sdk/include/ucrt /imsvc$M/sdk/include/um /imsvc$M/sdk/include/shared /DNOMINMAX /D_WIN32_WINNT=0x0A00 \
  /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /I$ROOT/recomp/runtime /I$HERE /I$OUT/gen"
# objects older than a header they may include are rebuilt too: the
# recompiled units only use the runtime's headers, the host files also these
NEWEST_H=$(ls -t "$ROOT"/recomp/runtime/*.h | head -1)
compile() {  # flags, sources...
    local flags="$1"; shift
    printf '%s\n' "$@" | xargs -P "$JOBS" -I{} sh -c \
        'o="'"$OUT"'/obj/$(basename {} .cpp).obj"; [ "$o" -nt {} ] && [ "$o" -nt "'"$NEWEST_H"'" ] || clang-cl-18 '"$flags"' {} /Fo"$o" > "$o.log" 2>&1 || { rm -f "$o"; echo "FAIL {}"; head -5 "$o.log"; exit 255; }'
}
compile "$SYS /Od" "$OUT"/gen/recomp_*.cpp
# glslang + SPIRV-Cross translate the plugin's GLSL to HLSL at runtime
TP="${THIRD_PARTY:?set THIRD_PARTY to the directory holding glslang and SPIRV-Cross}"
mkdir -p "$OUT/obj/tp"
TPSRC=$(ls "$TP"/glslang/glslang/MachineIndependent/*.cpp "$TP"/glslang/glslang/MachineIndependent/preprocessor/*.cpp \
    "$TP"/glslang/glslang/GenericCodeGen/*.cpp "$TP"/glslang/glslang/OSDependent/Windows/ossource.cpp \
    "$TP"/glslang/glslang/ResourceLimits/ResourceLimits.cpp "$TP"/glslang/SPIRV/GlslangToSpv.cpp "$TP"/glslang/SPIRV/InReadableOrder.cpp \
    "$TP"/glslang/SPIRV/Logger.cpp "$TP"/glslang/SPIRV/SpvBuilder.cpp "$TP"/glslang/SPIRV/SpvPostProcess.cpp "$TP"/glslang/SPIRV/doc.cpp \
    "$TP"/SPIRV-Cross/spirv_cross.cpp "$TP"/SPIRV-Cross/spirv_parser.cpp "$TP"/SPIRV-Cross/spirv_cross_parsed_ir.cpp \
    "$TP"/SPIRV-Cross/spirv_cfg.cpp "$TP"/SPIRV-Cross/spirv_glsl.cpp "$TP"/SPIRV-Cross/spirv_hlsl.cpp)
TPFLAGS="$SYS /O2 /I$TP/glslang /I$TP/SPIRV-Cross /DENABLE_SPIRV=1 /DGLSLANG_OSINCLUDE_WIN32"
compile "$TPFLAGS" $TPSRC
compile "$TPFLAGS" "$HERE/glsl_to_hlsl.cpp"
NEWEST_H=$(ls -t "$HERE"/*.h "$ROOT"/recomp/runtime/*.h | head -1)
compile "$SYS /O2" "$ROOT/recomp/runtime/recomp_runtime.cpp" "$ROOT/recomp/runtime/recomp_bridge.cpp" \
    "$ROOT/recomp/runtime/macho_image.cpp" "$OUT/gen/spfx_symbols.cpp" "$OUT/gen/spfx_exports.cpp" \
    "$HERE"/spfx_host.cpp "$HERE"/spfx_libc.cpp "$HERE"/spfx_cxx.cpp "$HERE"/spfx_gl.cpp "$HERE"/gl_d3d11.cpp \
    "$HERE"/spfx_runtime_glue.cpp
lld-link-18 /nologo /dll /out:"$OUT/SPFXUnityPlugin.dll" /map:"$OUT/SPFXUnityPlugin.map" /machine:x64 "$OUT"/obj/*.obj \
    /libpath:"$M/crt/lib/x86_64" /libpath:"$M/sdk/lib/um/x86_64" /libpath:"$M/sdk/lib/ucrt/x86_64" \
    libcmt.lib libvcruntime.lib libucrt.lib libcpmt.lib kernel32.lib user32.lib d3d11.lib d3dcompiler.lib dxguid.lib
echo "built $OUT/SPFXUnityPlugin.dll"
