# Generates spfx_exports.cpp: the SPFXUnityPlugin.dll exports the game's
# P/Invokes call (tools/spfx/externs.txt), each running the recompiled
# function of the same name with its arguments placed the way Apple's arm64
# ABI expects (x0-x7 / v0-v7, then the stack packed at natural alignment).
#
# usage: python3 -I gen_exports.py <externs.txt> <arm64 Mach-O> <out.cpp>
import re, sys

import lief

externs, binary, out = sys.argv[1:4]
macho = lief.parse(binary)
addr_of = {s.name: s.value for s in macho.symbols if s.value}

# C# type -> (C type, kind, size): kind is int, float or ptr
TYPES = {"IntPtr": ("void*", "int", 8), "int": ("int32_t", "int", 4), "uint": ("uint32_t", "int", 4),
         "float": ("float", "float", 4), "bool": ("int32_t", "int", 4), "string": ("const char*", "int", 8),
         "byte[]": ("const uint8_t*", "int", 8), "float[]": ("const float*", "int", 8),
         "IntPtr[]": ("void* const*", "int", 8)}
RETURNS = {"void": "void", "IntPtr": "void*", "int": "int32_t", "uint": "uint32_t", "bool": "int32_t"}

# host processing of arguments before the guest runs (gl_d3d11.cpp)
# Unity hands D3D11 texture pointers where the plugin's GL code expects texture
# names: they are swapped for names of emulated textures wrapping them.
HOOKS = {"SPFX_PLUGIN_Instance_Create": "TextureInfo = spfx::gl::NativeTextureNames(TextureInfo, TextureInfoCount);",
         "SPFX_PLUGIN_Instance_SetLabelTexture": "Texture = spfx::gl::NativeTextureName(Texture);",
         "SPFX_PLUGIN_SetDrawParameter": "spfx::gl::NoteDrawParameter(CameraID, Parameter);"}

# host bookkeeping after the guest returns (c.x[0] holds its result)
POSTHOOKS = {"SPFX_PLUGIN_Instance_Create": "spfx::NoteInstanceCreated(reinterpret_cast<void*>(c.x[0]), reinterpret_cast<const char*>(FileName)); spfx::NoteCoinCreated143(reinterpret_cast<void*>(c.x[0]), reinterpret_cast<const char*>(FileName), ResourceLength, InstanceID, TextureInfo, TextureInfoCount);",
             "SPFX_PLUGIN_Instance_GetStatus": "spfx::NoteInstanceStatus(Handle, int32_t(c.x[0]));"}

# exports the host wraps itself (spfx_host.cpp)
CUSTOM = {"SPFX_PLUGIN_GetRenderingCallback"}

lines = []
for line in open(externs):
    m = re.match(r"\s*(?:private |public )?static extern (\S+) (\w+)\((.*)\)", line)
    if not m or m.group(2) in CUSTOM:
        continue
    ret, name, params = m.groups()
    args = []
    for p in filter(None, (x.strip() for x in params.split(","))):
        t, n = p.rsplit(" ", 1)
        if t.startswith(("ref ", "out ")):
            args.append(("void*", "int", 8, n))
        else:
            args.append(TYPES[t] + (n,))
    guest = addr_of["_" + name]
    sig = ", ".join(f"{c} {n}" for c, _, _, n in args)
    # without the macOS plugin image the DLL answers like the game's null plugin
    fallback = {"SPFX_PLUGIN_Instance_Create": "spfx::FallbackHandle()", "SPFX_PLUGIN_Instance_GetStatus": "1"}.get(name, "0")
    r0 = RETURNS.get(ret, "void")
    fb = "return;" if r0 == "void" else (f"return reinterpret_cast<void*>({fallback});" if r0 == "void*" and fallback != "0" else f"return {r0}({fallback if r0 != 'void*' else 'nullptr'});")
    if r0 == "void*" and fallback == "0":
        fb = "return nullptr;"
    body = [f"    if (!spfx::EnsureLoaded()) {fb}"]
    if name in HOOKS:
        body.append(f"    {HOOKS[name]}")
    body += ["    GuestStackScope scope;", "    Ctx c;", "    memset(&c, 0, sizeof(c));"]
    ngrn = nsrn = 0
    stack = 0
    stack_writes = []
    for c, kind, size, n in args:
        if kind == "float" and nsrn < 8:
            body.append(f"    SetS(c.v[{nsrn}], {n});")
            nsrn += 1
        elif kind != "float" and ngrn < 8:
            body.append(f"    c.x[{ngrn}] = uint64_t({'reinterpret_cast<uintptr_t>(' + n + ')' if c.endswith('*') else n});")
            ngrn += 1
        else:
            stack = (stack + size - 1) & ~(size - 1)
            val = f"reinterpret_cast<uintptr_t>({n})" if c.endswith("*") else n
            stack_writes.append(f"    {{ auto v = {'uint64_t' if size == 8 else c}({val}); memcpy(reinterpret_cast<void*>(c.sp + {stack}), &v, {size}); }}")
            stack += size
    frame = (stack + 15) & ~15
    body.append(f"    c.sp = scope.Top() - {frame};")
    body += stack_writes
    fmt = ", ".join(("%g" if kind == "float" else "0x%llx") for _, kind, _, _ in args)
    vals = "".join(f", double({n})" if kind == "float" else (f", (unsigned long long)reinterpret_cast<uintptr_t>({n})" if c.endswith("*") else f", (unsigned long long)uint64_t({n})") for c, kind, _, n in args)
    trace_condition = "spfx::TraceCalls()"
    if name.startswith("SPFX_PLUGIN_Instance_") and any(n == "Handle" for _, _, _, n in args) and "Get" not in name:
        trace_condition += " || spfx::TraceCoinHandle143(Handle)"
    body.append(f'    if ({trace_condition}) spfx::Log("{name}({fmt})"{vals});')
    if name == "SPFX_PLUGIN_Instance_SetTransform":
        body.append('    if (WorldMatrix && spfx::TraceCoinHandle143(Handle)) spfx::Log("COIN matrix %p scale %g %g %g pos %g %g %g", Handle, double(WorldMatrix[0]), double(WorldMatrix[5]), double(WorldMatrix[10]), double(WorldMatrix[12]), double(WorldMatrix[13]), double(WorldMatrix[14]));')
    body.append(f"    f_{guest:x}(c);")
    if name in POSTHOOKS:
        body.append(f"    {POSTHOOKS[name]}")
    if ret != "void":
        body.append(f'    if (spfx::TraceCalls()) spfx::Log("  -> 0x%llx", (unsigned long long)c.x[0]);')
    r = RETURNS.get(ret, "void")
    if ret == "bool":
        body.append("    return int32_t(uint8_t(c.x[0]));")
    elif r == "void*":
        body.append("    return reinterpret_cast<void*>(c.x[0]);")
    elif r != "void":
        body.append(f"    return {r}(c.x[0]);")
    lines.append(f"EXPORT {r} {name}({sig})\n{{\n" + "\n".join(body) + "\n}\n")

with open(out, "w") as f:
    f.write("""// Generated by plugins/spfx/gen_exports.py
#include <cstdint>
#include <cstring>

#include "recomp_bridge.h"
#include "recomp_decls.h"
#include "recomp_simd.h"
#include "spfx_host.h"

namespace spfx::gl
{
    void* const* NativeTextureNames(void* const* info, int32_t count);
    void* NativeTextureName(void* texture);
    void NoteDrawParameter(int32_t camera, const void* parameter);
}

#define EXPORT extern "C" __declspec(dllexport)

using namespace recomp;

""")
    f.write("\n".join(lines))
print(f"{len(lines)} exports")
