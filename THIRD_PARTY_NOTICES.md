# Third-party notices

The GoSRecomp source code is under the license in [LICENSE](LICENSE). Builds and releases also contain or use the following.

## Built into the binaries

| Component | Where | License |
|---|---|---|
| libil2cpp, Boehm GC, zlib and baselib headers from Unity 2020.3.49f1 (1.4.3 branch) | privately built `GameAssembly.dll` | Unity Technologies' terms for the IL2CPP runtime sources (Boehm GC: MIT-style; zlib: zlib license) |
| [glslang](https://github.com/KhronosGroup/glslang) 15.4.0 | `SPFXUnityPlugin.dll` | BSD 3-Clause, BSD 2-Clause, MIT and Apache 2.0 parts (see its LICENSE.txt) |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) vulkan-sdk-1.3.296.0 | `SPFXUnityPlugin.dll` | Apache License 2.0 |
| Microsoft C/C++ runtime (static) | all DLLs | Microsoft Visual C++ redistributable terms |

## Shipped alongside

| Component | Where | License |
|---|---|---|
| Unity 2020.3.49f1 Windows player (`GoS.exe`, `UnityPlayer.dll`, `baselib.dll`) | owner's private installation; not distributed by this branch | Unity Technologies' terms |
| [7-Zip](https://www.7-zip.org/) (`7z.exe`, `7z.dll`) | `_tools` | GNU LGPL with the unRAR restriction (see `_tools/License.txt`) |

## Used by the build only

Il2CppDumper (MIT), LIEF (Apache 2.0), Capstone (BSD 3-Clause), Unicorn (GPL 2), UnityPy (MIT), xwin (MIT/Apache 2.0), LLVM/clang (Apache 2.0 with LLVM exceptions).
