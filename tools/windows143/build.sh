#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build.sh OUTSIDE_CHECKOUT_TOOLS_DIRECTORY}
M=${MSVC_SYSROOT:?set MSVC_SYSROOT}
mkdir -p "$OUT"
for unit in isolated_launch network_probe; do
 clang-cl-18 --target=x86_64-pc-windows-msvc -fuse-ld=lld /nologo /EHsc /MT /O2 /std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH \
  /imsvc"$M/crt/include" /imsvc"$M/sdk/include/ucrt" /imsvc"$M/sdk/include/um" /imsvc"$M/sdk/include/shared" \
  "$HERE/$unit.cpp" /Fo"$OUT/$unit.obj" /Fe"$OUT/$unit.exe" \
  /link /libpath:"$M/crt/lib/x86_64" /libpath:"$M/sdk/lib/um/x86_64" /libpath:"$M/sdk/lib/ucrt/x86_64" kernel32.lib ws2_32.lib
done
