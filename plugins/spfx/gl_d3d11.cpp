// Draws the emulated GL context's work with Unity's Direct3D 11 device.
//
// Programs are translated (glsl_to_hlsl.cpp) and compiled on worker threads
// from link time, with a disk cache (gosr_spfx_cache);
// reflection gives each uniform's constant buffer offset and each sampler's
// texture/sampler registers. GL buffers and textures live in host memory
// (gl_state.h) and get D3D11 copies when their contents change. Draws go to
// whatever render target Unity bound for the plugin event.
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include "gl_state.h"
#include "glsl_to_hlsl.h"
#include "spfx_host.h"
#include "unity_interfaces.h"

extern "C" void SPFX_PLUGIN_SetDrawParameter(int32_t camera, void* parameter);  // spfx_exports.cpp

namespace spfx::gl
{
    std::mutex g_coinTexturesLock143;
    std::map<uint32_t, bool> g_coinTextures143;
    void MarkCoinTexture143(uint32_t texture)
    {
        std::lock_guard<std::mutex> lock(g_coinTexturesLock143);
        if (g_coinTextures143.size() < 128) g_coinTextures143[texture] = true;
    }
    bool CoinDraw143(const Program& program, const State& state)
    {
        std::lock_guard<std::mutex> lock(g_coinTexturesLock143);
        for (const Uniform& u : program.uniforms)
            if (u.type && uint32_t(u.samplerUnit) < kMaxUnits && g_coinTextures143.count(state.unit[uint32_t(u.samplerUnit)].texture2d)) return true;
        return false;
    }
    State& GetState()
    {
        static State state;
        return state;
    }

    namespace
    {
        template <typename T>
        struct Com
        {
            T* p = nullptr;
            Com() = default;
            Com(const Com&) = delete;
            Com& operator=(const Com&) = delete;
            ~Com() { if (p) p->Release(); }
            T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
            T* get() const { return p; }
            T* operator->() const { return p; }
            explicit operator bool() const { return p != nullptr; }
        };

        ID3D11Device* g_device = nullptr;
        ID3D11DeviceContext* g_context = nullptr;
        bool g_flipCameraY = false;  // while a camera is drawn with its projection flipped (BeginCameraDraw)
        bool g_unavailable = false;

        bool EnsureDevice()
        {
            if (g_device)
                return true;
            if (g_unavailable)
                return false;
            IUnityInterfaces* ui = spfx::RealInterfaces();
            auto* d3d = ui ? ui->Get<IUnityGraphicsD3D11>() : nullptr;
            g_device = d3d ? d3d->GetDevice() : nullptr;
            if (!g_device)
            {
                g_unavailable = true;
                Log("no Direct3D 11 device: effects are not drawn");
                return false;
            }
            g_device->AddRef();
            g_device->GetImmediateContext(&g_context);
            Log("drawing with Unity's Direct3D 11 device");
            return true;
        }

        // ------------------------------------------------------------ programs

        struct UniformSlot
        {
            int vsOffset = -1, psOffset = -1;  // bytes in each stage's constant buffer
            int vsTexture = -1, vsSampler = -1, psTexture = -1, psSampler = -1;
        };

        struct GpuProgram
        {
            Com<ID3D11VertexShader> vs;
            Com<ID3D11PixelShader> ps;
            Com<ID3DBlob> vsCode;
            Com<ID3D11Buffer> vsCb, psCb;
            uint32_t vsCbSize = 0, psCbSize = 0;
            std::vector<UniformSlot> slots;  // by uniform index
            std::vector<std::string> attributes;
            std::vector<uint8_t> vsData, psData;
            bool failed = false;
        };

        // "_131_g_VS_WorldMatrix" -> "g_VS_WorldMatrix" (SPIRV-Cross prefixes block members)
        std::string MemberName(const char* n)
        {
            static const std::regex prefix(R"(^_\d+_)");
            return std::regex_replace(std::string(n), prefix, "");
        }

        void Reflect(ID3DBlob* code, Program& p, GpuProgram& g, bool vertex)
        {
            Com<ID3D11ShaderReflection> r;
            if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_ID3D11ShaderReflection,
                                  reinterpret_cast<void**>(r.put()))))
                return;
            std::map<std::string, size_t> index;
            for (size_t i = 0; i < p.uniforms.size(); i++)
                index[p.uniforms[i].name] = i;
            D3D11_SHADER_DESC desc;
            r->GetDesc(&desc);
            for (UINT b = 0; b < desc.ConstantBuffers; b++)
            {
                ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(b);
                D3D11_SHADER_BUFFER_DESC cbDesc;
                cb->GetDesc(&cbDesc);
                (vertex ? g.vsCbSize : g.psCbSize) = cbDesc.Size;
                for (UINT v = 0; v < cbDesc.Variables; v++)
                {
                    D3D11_SHADER_VARIABLE_DESC vd;
                    cb->GetVariableByIndex(v)->GetDesc(&vd);
                    auto it = index.find(MemberName(vd.Name));
                    if (it != index.end())
                        (vertex ? g.slots[it->second].vsOffset : g.slots[it->second].psOffset) = int(vd.StartOffset);
                }
            }
            for (UINT i = 0; i < desc.BoundResources; i++)
            {
                D3D11_SHADER_INPUT_BIND_DESC bd;
                r->GetResourceBindingDesc(i, &bd);
                std::string n = bd.Name;
                bool sampler = bd.Type == D3D_SIT_SAMPLER;
                if (sampler)  // "_g_SamplerColor1_sampler"
                    n = n.substr(1, n.size() - 1 - strlen("_sampler"));
                auto it = index.find(n);
                if (it == index.end() || bd.Type == D3D_SIT_CBUFFER)
                    continue;
                UniformSlot& s = g.slots[it->second];
                if (vertex)
                    (sampler ? s.vsSampler : s.vsTexture) = int(bd.BindPoint);
                else
                    (sampler ? s.psSampler : s.psTexture) = int(bd.BindPoint);
            }
        }

        // Shader builds (GLSL -> HLSL -> DXBC) run on worker threads, starting
        // when the plugin links a program, and land in a disk cache so effects
        // seen once never compile again. A program whose build is not done yet
        // is skipped for that frame instead of stalling the render thread.
        struct ShaderBuild
        {
            std::atomic<int> state{ 0 };  // 0 pending, 1 ready, 2 failed
            std::string vsCode, psCode;   // DXBC
            std::vector<std::string> attributes;
            uint32_t name = 0;
            std::string vsGlsl, fsGlsl;
        };

        constexpr uint32_t kCacheVersion = 1;  // bump when the translation changes

        uint64_t Hash(const std::string& a, const std::string& b)
        {
            uint64_t h = 0xcbf29ce484222325ull ^ kCacheVersion;
            for (const std::string* s : { &a, &b })
            {
                for (unsigned char ch : *s)
                    h = (h ^ ch) * 0x100000001b3ull;
                h = (h ^ 0xFF) * 0x100000001b3ull;
            }
            return h;
        }

        std::string CachePath(uint64_t key)
        {
            char path[64];
            snprintf(path, sizeof path, "gosr_spfx_cache/%016llx.bin", static_cast<unsigned long long>(key));
            return path;
        }

        bool ReadCache(uint64_t key, ShaderBuild& b)
        {
            FILE* f = fopen(CachePath(key).c_str(), "rb");
            if (!f)
                return false;
            auto readString = [f](std::string& out) {
                uint32_t n = 0;
                if (fread(&n, 4, 1, f) != 1 || n > (64u << 20))
                    return false;
                out.resize(n);
                return n == 0 || fread(out.data(), 1, n, f) == n;
            };
            uint32_t magic = 0, count = 0;
            bool ok = fread(&magic, 4, 1, f) == 1 && magic == 0x31435347 && readString(b.vsCode) && readString(b.psCode) &&
                      fread(&count, 4, 1, f) == 1 && count < 64;
            b.attributes.resize(ok ? count : 0);
            for (auto& a : b.attributes)
                ok = ok && readString(a);
            fclose(f);
            return ok;
        }

        void WriteCache(uint64_t key, const ShaderBuild& b)
        {
            CreateDirectoryA("gosr_spfx_cache", nullptr);
            std::string path = CachePath(key), temp = path + ".tmp";
            FILE* f = fopen(temp.c_str(), "wb");
            if (!f)
                return;
            auto writeString = [f](const std::string& s) {
                uint32_t n = uint32_t(s.size());
                fwrite(&n, 4, 1, f);
                fwrite(s.data(), 1, n, f);
            };
            uint32_t magic = 0x31435347, count = uint32_t(b.attributes.size());
            fwrite(&magic, 4, 1, f);
            writeString(b.vsCode);
            writeString(b.psCode);
            fwrite(&count, 4, 1, f);
            for (const auto& a : b.attributes)
                writeString(a);
            bool ok = !ferror(f);
            fclose(f);
            if (!ok || !MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                DeleteFileA(temp.c_str());
        }

        bool Compile(const std::string& src, const char* profile, std::string& code, uint32_t name)
        {
            Com<ID3DBlob> blob, errors;
            HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put());
            if (FAILED(hr))
            {
                Log("program %u: %s failed: %s", name, profile, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
                return false;
            }
            code.assign(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
            return true;
        }

        void Build(uint64_t key, ShaderBuild& b)
        {
            HlslProgram hlsl;
            bool translated;
            {
                static std::mutex translateLock;  // one glslang/SPIRV-Cross run at a time
                std::lock_guard<std::mutex> guard(translateLock);
                translated = TranslateProgram(b.vsGlsl, b.fsGlsl, hlsl);
            }
            if (!translated)
            {
                Log("program %u: translation failed: %s %s", b.name, hlsl.vertex.error.c_str(), hlsl.pixel.error.c_str());
                b.state = 2;
                return;
            }
            if (!Compile(hlsl.vertex.source, "vs_5_0", b.vsCode, b.name) || !Compile(hlsl.pixel.source, "ps_5_0", b.psCode, b.name))
            {
                b.state = 2;
                return;
            }
            b.attributes = hlsl.attributes;
            WriteCache(key, b);
            b.state = 1;
        }

        struct BuildQueue
        {
            std::mutex lock;
            std::condition_variable wake;
            std::deque<std::pair<uint64_t, std::shared_ptr<ShaderBuild>>> jobs;
            std::unordered_map<uint64_t, std::shared_ptr<ShaderBuild>> builds;  // by source hash
            bool started = false;

            void Worker()
            {
                for (;;)
                {
                    std::pair<uint64_t, std::shared_ptr<ShaderBuild>> job;
                    {
                        std::unique_lock<std::mutex> guard(lock);
                        wake.wait(guard, [this] { return !jobs.empty(); });
                        job = jobs.front();
                        jobs.pop_front();
                    }
                    Build(job.first, *job.second);
                }
            }
        };

        BuildQueue& Builds()
        {
            static BuildQueue* q = new BuildQueue;  // workers outlive static destruction
            return *q;
        }

        // The build of a program's sources: shared by identical programs,
        // loaded from the disk cache or queued for the workers.
        std::shared_ptr<ShaderBuild> RequestBuild(uint32_t name, const Program& p)
        {
            BuildQueue& q = Builds();
            uint64_t key = Hash(p.vertexSource, p.fragmentSource);
            std::lock_guard<std::mutex> guard(q.lock);
            auto& b = q.builds[key];
            if (b)
                return b;
            b = std::make_shared<ShaderBuild>();
            b->name = name;
            if (ReadCache(key, *b))
            {
                b->state = 1;
                return b;
            }
            b->vsGlsl = p.vertexSource;
            b->fsGlsl = p.fragmentSource;
            q.jobs.emplace_back(key, b);
            if (!q.started)
            {
                q.started = true;
                unsigned n = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 2));
                wchar_t workers[8];
                if (GetEnvironmentVariableW(L"GOSR_SPFX_WORKERS", workers, 8))
                    n = std::max(1, _wtoi(workers));
                for (unsigned i = 0; i < n; i++)
                    std::thread([&q] { q.Worker(); }).detach();
            }
            q.wake.notify_one();
            return b;
        }

        GpuProgram* Prepare(uint32_t name, Program& p)
        {
            if (!p.gpu)
            {
                if (!p.build)
                    p.build = RequestBuild(name, p);
                auto* b = static_cast<ShaderBuild*>(p.build.get());
                int state = b->state;
                if (state == 0)
                {
                    static const bool wait = GetEnvironmentVariableW(L"GOSR_SPFX_SYNC_SHADERS", nullptr, 0) != 0;
                    if (!wait)
                        return nullptr;  // not built yet: skip this draw
                    while ((state = b->state) == 0)
                        Sleep(1);
                }
                auto* g = new GpuProgram;
                p.gpu = g;
                if (state != 1)
                {
                    g->failed = true;
                    return nullptr;
                }
                Com<ID3DBlob> vsCode, psCode;
                D3DCreateBlob(b->vsCode.size(), g->vsCode.put());
                memcpy(g->vsCode->GetBufferPointer(), b->vsCode.data(), b->vsCode.size());
                D3DCreateBlob(b->psCode.size(), psCode.put());
                memcpy(psCode->GetBufferPointer(), b->psCode.data(), b->psCode.size());
                if (FAILED(g_device->CreateVertexShader(g->vsCode->GetBufferPointer(), g->vsCode->GetBufferSize(), nullptr, g->vs.put())) ||
                    FAILED(g_device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, g->ps.put())))
                {
                    Log("program %u: cannot create its shaders", name);
                    g->failed = true;
                    return nullptr;
                }
                g->slots.resize(p.uniforms.size());
                Reflect(g->vsCode.get(), p, *g, true);
                Reflect(psCode.get(), p, *g, false);
                g->attributes = b->attributes;
                for (int stage = 0; stage < 2; stage++)
                {
                    uint32_t size = stage ? g->psCbSize : g->vsCbSize;
                    if (!size)
                        continue;
                    D3D11_BUFFER_DESC bd = {};
                    bd.ByteWidth = (size + 15) & ~15u;
                    bd.Usage = D3D11_USAGE_DYNAMIC;
                    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    g_device->CreateBuffer(&bd, nullptr, stage ? g->psCb.put() : g->vsCb.put());
                    (stage ? g->psData : g->vsData).assign(bd.ByteWidth, 0);
                }
            }
            auto* g = static_cast<GpuProgram*>(p.gpu);
            return g->failed ? nullptr : g;
        }

        void Upload(ID3D11Buffer* cb, const std::vector<uint8_t>& data)
        {
            D3D11_MAPPED_SUBRESOURCE m;
            if (cb && SUCCEEDED(g_context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, data.data(), data.size());
                g_context->Unmap(cb, 0);
            }
        }

        // ------------------------------------------------------------ buffers

        struct GpuBuffer
        {
            Com<ID3D11Buffer> buffer;
            uint64_t version = ~0ull;
            UINT size = 0;
            UINT bind = 0;
        };
        std::unordered_map<uint32_t, GpuBuffer> g_buffers;

        ID3D11Buffer* BufferFor(uint32_t name, UINT bind)
        {
            auto it = GetState().buffers.find(name);
            if (it == GetState().buffers.end() || it->second.data.empty())
                return nullptr;
            const Buffer& b = it->second;
            GpuBuffer& g = g_buffers[name];
            UINT size = UINT((b.data.size() + 15) & ~size_t(15));
            if (g.buffer && g.version == b.version && (g.bind & bind) == bind)
                return g.buffer.get();
            if (!g.buffer || g.size < size || (g.bind & bind) != bind)
            {
                D3D11_BUFFER_DESC bd = {};
                bd.ByteWidth = size;
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                g_device->CreateBuffer(&bd, nullptr, g.buffer.put());
                g.size = size;
                g.bind = bd.BindFlags;
            }
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_context->Map(g.buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, b.data.data(), b.data.size());
                g_context->Unmap(g.buffer.get(), 0);
            }
            g.version = b.version;
            return g.buffer.get();
        }

        // client-memory vertex data or attributes D3D cannot read (integer as float): float copies
        struct Scratch
        {
            Com<ID3D11Buffer> buffer;
            UINT size = 0;
        };
        Scratch g_scratch[kMaxAttribs];
        Com<ID3D11Buffer> g_defaultAttrib;

        ID3D11Buffer* ScratchBuffer(uint32_t slot, const void* data, UINT bytes)
        {
            Scratch& s = g_scratch[slot];
            if (!s.buffer || s.size < bytes)
            {
                D3D11_BUFFER_DESC bd = {};
                bd.ByteWidth = (bytes + 4095) & ~4095u;
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                g_device->CreateBuffer(&bd, nullptr, s.buffer.put());
                s.size = bd.ByteWidth;
            }
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_context->Map(s.buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                memcpy(m.pData, data, bytes);
                g_context->Unmap(s.buffer.get(), 0);
            }
            return s.buffer.get();
        }

        uint32_t TypeSize(uint32_t type)
        {
            switch (type)
            {
            case 0x1400: case 0x1401: return 1;  // BYTE, UNSIGNED_BYTE
            case 0x1402: case 0x1403: case 0x140B: return 2;  // SHORT, UNSIGNED_SHORT, HALF_FLOAT
            default: return 4;  // INT, UNSIGNED_INT, FLOAT
            }
        }

        // D3D formats for GL attributes the input assembler reads directly
        DXGI_FORMAT DirectFormat(const Attrib& a)
        {
            static const DXGI_FORMAT f32[4] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
            static const DXGI_FORMAT unorm8[4] = { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM };
            static const DXGI_FORMAT snorm8[4] = { DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_SNORM };
            static const DXGI_FORMAT unorm16[4] = { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_UNORM };
            static const DXGI_FORMAT snorm16[4] = { DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_SNORM };
            static const DXGI_FORMAT half[4] = { DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT };
            int i = a.size - 1;
            if (i < 0 || i > 3)
                return DXGI_FORMAT_UNKNOWN;
            switch (a.type)
            {
            case 0x1406: return f32[i];
            case 0x140B: return half[i];
            case 0x1401: return a.normalized ? unorm8[i] : DXGI_FORMAT_UNKNOWN;
            case 0x1400: return a.normalized ? snorm8[i] : DXGI_FORMAT_UNKNOWN;
            case 0x1403: return a.normalized ? unorm16[i] : DXGI_FORMAT_UNKNOWN;
            case 0x1402: return a.normalized ? snorm16[i] : DXGI_FORMAT_UNKNOWN;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        float ReadComponent(const uint8_t* p, uint32_t type, bool normalized)
        {
            switch (type)
            {
            case 0x1400: { int8_t v; memcpy(&v, p, 1); return normalized ? std::max(float(v) / 127.0f, -1.0f) : float(v); }
            case 0x1401: return normalized ? float(*p) / 255.0f : float(*p);
            case 0x1402: { int16_t v; memcpy(&v, p, 2); return normalized ? std::max(float(v) / 32767.0f, -1.0f) : float(v); }
            case 0x1403: { uint16_t v; memcpy(&v, p, 2); return normalized ? float(v) / 65535.0f : float(v); }
            case 0x1404: { int32_t v; memcpy(&v, p, 4); return float(v); }
            case 0x1405: { uint32_t v; memcpy(&v, p, 4); return float(v); }
            default: { float v; memcpy(&v, p, 4); return v; }
            }
        }

        // ------------------------------------------------------------ textures

        struct GpuTexture
        {
            Com<ID3D11Texture2D> texture;
            Com<ID3D11ShaderResourceView> srv;
            uint64_t version = ~0ull;
        };

        DXGI_FORMAT CompressedFormat(uint32_t internal, uint32_t& blockBytes)
        {
            switch (internal)
            {
            case 0x83F0: case 0x83F1: blockBytes = 8; return DXGI_FORMAT_BC1_UNORM;   // DXT1 RGB / RGBA
            case 0x83F2: blockBytes = 16; return DXGI_FORMAT_BC2_UNORM;                // DXT3
            case 0x83F3: blockBytes = 16; return DXGI_FORMAT_BC3_UNORM;                // DXT5
            default: blockBytes = 0; return DXGI_FORMAT_UNKNOWN;
            }
        }

        // uncompressed images become RGBA8 (or R8/A8 where D3D samples alike)
        DXGI_FORMAT ExpandedFormat(uint32_t internal, int& channels)
        {
            switch (internal)
            {
            case 0x1903: case 0x8229: channels = 1; return DXGI_FORMAT_R8_UNORM;   // RED, R8
            case 0x1906: channels = 1; return DXGI_FORMAT_A8_UNORM;                // ALPHA
            case 0x1909: channels = 1; return DXGI_FORMAT_R8G8B8A8_UNORM;          // LUMINANCE -> L,L,L,1
            case 0x190A: channels = 2; return DXGI_FORMAT_R8G8B8A8_UNORM;          // LUMINANCE_ALPHA
            case 0x1907: case 0x8051: channels = 3; return DXGI_FORMAT_R8G8B8A8_UNORM;  // RGB
            default: channels = 4; return DXGI_FORMAT_R8G8B8A8_UNORM;
            }
        }

        ID3D11ShaderResourceView* g_white = nullptr;

        ID3D11ShaderResourceView* WhiteTexture()
        {
            if (!g_white)
            {
                uint32_t px = 0xFFFFFFFF;
                D3D11_TEXTURE2D_DESC td = { 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
                D3D11_SUBRESOURCE_DATA sd = { &px, 4, 4 };
                ID3D11Texture2D* t = nullptr;
                g_device->CreateTexture2D(&td, &sd, &t);
                if (t)
                {
                    g_device->CreateShaderResourceView(t, nullptr, &g_white);
                    t->Release();
                }
            }
            return g_white;
        }

        ID3D11ShaderResourceView* TextureView(uint32_t name)
        {
            auto it = GetState().textures.find(name);
            if (it == GetState().textures.end())
                return WhiteTexture();
            Texture& t = it->second;
            if (!t.gpu)
                t.gpu = new GpuTexture;
            auto* g = static_cast<GpuTexture*>(t.gpu);
            if (t.external)
            {
                if (!g->srv)
                {
                    Com<ID3D11Resource> res;
                    static_cast<IUnknown*>(t.external)->QueryInterface(IID_ID3D11Resource, reinterpret_cast<void**>(res.put()));
                    if (!res || FAILED(g_device->CreateShaderResourceView(res.get(), nullptr, g->srv.put())))
                    {
                        Log("Unity texture %p has no shader view", t.external);
                        return WhiteTexture();
                    }
                }
                return g->srv.get();
            }
            if (t.screenCopy)
                return g->srv ? g->srv.get() : WhiteTexture();
            if (g->version == t.version && g->srv)
                return g->srv.get();
            if (t.levels.empty() || !t.width || !t.height)
                return WhiteTexture();
            uint32_t blockBytes = 0;
            DXGI_FORMAT format = t.compressed ? CompressedFormat(t.internalFormat, blockBytes) : DXGI_FORMAT_UNKNOWN;
            int channels = 4;
            if (!t.compressed)
                format = ExpandedFormat(t.internalFormat, channels);
            if (format == DXGI_FORMAT_UNKNOWN)
            {
                static int logged = 0;
                if (logged++ < 10)
                    Log("texture format 0x%x not supported", t.internalFormat);
                return WhiteTexture();
            }
            // complete mip chains only (GL samples incomplete textures as black; use level 0)
            UINT levels = UINT(t.levels.size());
            for (UINT l = 1; l < levels; l++)
                if (t.levels[l].empty())
                    levels = l;
            std::vector<D3D11_SUBRESOURCE_DATA> sd(levels);
            std::vector<std::vector<uint8_t>> expanded(levels);
            for (UINT l = 0; l < levels; l++)
            {
                UINT w = std::max(1u, t.width >> l), h = std::max(1u, t.height >> l);
                if (t.compressed)
                {
                    sd[l] = { t.levels[l].data(), ((w + 3) / 4) * blockBytes, 0 };
                    continue;
                }
                const uint8_t* src = t.levels[l].data();
                if (channels == 1 && format != DXGI_FORMAT_R8G8B8A8_UNORM)
                {
                    expanded[l].assign(src, src + size_t(w) * h);
                    sd[l] = { expanded[l].data(), w, 0 };
                    continue;
                }
                expanded[l].resize(size_t(w) * h * 4);
                uint8_t* dst = expanded[l].data();
                size_t n = size_t(w) * h;
                if (t.levels[l].size() < n * size_t(channels))
                    continue;
                for (size_t i = 0; i < n; i++)
                {
                    const uint8_t* s = src + i * size_t(channels);
                    uint8_t* d = dst + i * 4;
                    switch (channels)
                    {
                    case 1: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
                    case 2: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
                    case 3: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
                    default: memcpy(d, s, 4); break;
                    }
                }
                sd[l] = { dst, w * 4, 0 };
            }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = t.width;
            td.Height = t.height;
            td.MipLevels = levels;
            td.ArraySize = 1;
            td.Format = format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_device->CreateTexture2D(&td, sd.data(), g->texture.put())) ||
                FAILED(g_device->CreateShaderResourceView(g->texture.get(), nullptr, g->srv.put())))
            {
                Log("texture %u (%ux%u, 0x%x) could not be created", name, t.width, t.height, t.internalFormat);
                return WhiteTexture();
            }
            g->version = t.version;
            return g->srv.get();
        }

        // ------------------------------------------------------------ fixed-function state

        template <typename K, typename V>
        V* Cached(std::map<K, Com<V>>& cache, const K& key)
        {
            auto it = cache.find(key);
            return it == cache.end() ? nullptr : it->second.get();
        }

        D3D11_TEXTURE_ADDRESS_MODE Wrap(float v)
        {
            switch (uint32_t(v))
            {
            case 0x812F: return D3D11_TEXTURE_ADDRESS_CLAMP;   // CLAMP_TO_EDGE
            case 0x8370: return D3D11_TEXTURE_ADDRESS_MIRROR;  // MIRRORED_REPEAT
            case 0x812D: return D3D11_TEXTURE_ADDRESS_BORDER;  // CLAMP_TO_BORDER
            default: return D3D11_TEXTURE_ADDRESS_WRAP;
            }
        }

        ID3D11SamplerState* SamplerFor(const std::map<uint32_t, float>& params, bool mipmapped)
        {
            auto get = [&](uint32_t k, float d) { auto it = params.find(k); return it == params.end() ? d : it->second; };
            uint32_t minF = uint32_t(get(0x2801, 0x2702)), magF = uint32_t(get(0x2800, 0x2601));
            float aniso = get(0x84FE, 1.0f);
            uint64_t key = uint64_t(minF) | uint64_t(magF) << 16 | uint64_t(uint32_t(get(0x2802, 0x2901))) << 32 |
                           uint64_t(uint32_t(get(0x2803, 0x2901)) & 0xFFFF) << 48;
            key ^= uint64_t(mipmapped) << 63 ^ uint64_t(aniso > 1.0f) << 62;
            static std::map<uint64_t, Com<ID3D11SamplerState>> cache;
            if (auto* s = Cached(cache, key))
                return s;
            bool linMag = magF == 0x2601;
            bool linMin = minF == 0x2601 || minF == 0x2701 || minF == 0x2703;
            bool linMip = minF == 0x2702 || minF == 0x2703;
            bool useMip = mipmapped && minF >= 0x2700;
            D3D11_SAMPLER_DESC sd = {};
            int f = (linMin ? 0x10 : 0) | (linMag ? 0x4 : 0) | (useMip && linMip ? 0x1 : 0);
            sd.Filter = aniso > 1.0f && useMip ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER(f);
            sd.AddressU = Wrap(get(0x2802, 0x2901));
            sd.AddressV = Wrap(get(0x2803, 0x2901));
            sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sd.MaxAnisotropy = UINT(std::min(16.0f, std::max(1.0f, aniso)));
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MinLOD = 0;
            sd.MaxLOD = useMip ? D3D11_FLOAT32_MAX : 0;
            g_device->CreateSamplerState(&sd, cache[key].put());
            return cache[key].get();
        }

        D3D11_BLEND BlendFactor(uint32_t f)
        {
            switch (f)
            {
            case 0: return D3D11_BLEND_ZERO;
            case 1: return D3D11_BLEND_ONE;
            case 0x0300: return D3D11_BLEND_SRC_COLOR;
            case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
            case 0x0302: return D3D11_BLEND_SRC_ALPHA;
            case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
            case 0x0304: return D3D11_BLEND_DEST_ALPHA;
            case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
            case 0x0306: return D3D11_BLEND_DEST_COLOR;
            case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
            case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
            default: return D3D11_BLEND_ONE;
            }
        }

        D3D11_BLEND AlphaBlendFactor(uint32_t f)
        {
            switch (f)
            {
            case 0x0300: return D3D11_BLEND_SRC_ALPHA;
            case 0x0301: return D3D11_BLEND_INV_SRC_ALPHA;
            case 0x0306: return D3D11_BLEND_DEST_ALPHA;
            case 0x0307: return D3D11_BLEND_INV_DEST_ALPHA;
            default: return BlendFactor(f);
            }
        }

        D3D11_BLEND_OP BlendOp(uint32_t e)
        {
            switch (e)
            {
            case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
            case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
            case 0x8007: return D3D11_BLEND_OP_MIN;
            case 0x8008: return D3D11_BLEND_OP_MAX;
            default: return D3D11_BLEND_OP_ADD;
            }
        }

        D3D11_COMPARISON_FUNC Compare(uint32_t f)
        {
            switch (f)
            {
            case 0x0200: return D3D11_COMPARISON_NEVER;
            case 0x0201: return D3D11_COMPARISON_LESS;
            case 0x0202: return D3D11_COMPARISON_EQUAL;
            case 0x0203: return D3D11_COMPARISON_LESS_EQUAL;
            case 0x0204: return D3D11_COMPARISON_GREATER;
            case 0x0205: return D3D11_COMPARISON_NOT_EQUAL;
            case 0x0206: return D3D11_COMPARISON_GREATER_EQUAL;
            default: return D3D11_COMPARISON_ALWAYS;
            }
        }

        D3D11_STENCIL_OP StencilOp(uint32_t o)
        {
            switch (o)
            {
            case 0: return D3D11_STENCIL_OP_ZERO;
            case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
            case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
            case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
            case 0x150A: return D3D11_STENCIL_OP_INVERT;
            case 0x8507: return D3D11_STENCIL_OP_INCR;
            case 0x8508: return D3D11_STENCIL_OP_DECR;
            default: return D3D11_STENCIL_OP_KEEP;
            }
        }

        void ApplyFixedFunction(State& s, bool coinDraw143)
        {
            {
                uint64_t key = uint64_t(s.blend) | uint64_t(s.colorMask) << 1 | uint64_t(s.srcRgb & 0xFFFF) << 8 |
                               uint64_t(s.dstRgb & 0xFFFF) << 24 | uint64_t(s.blendEqRgb & 0xFFF) << 40;
                uint64_t key2 = uint64_t(s.srcAlpha & 0xFFFF) | uint64_t(s.dstAlpha & 0xFFFF) << 16 | uint64_t(s.blendEqAlpha & 0xFFFF) << 32;
                static std::map<std::pair<uint64_t, uint64_t>, Com<ID3D11BlendState>> cache;
                auto k = std::make_pair(key, key2);
                ID3D11BlendState* b = Cached(cache, k);
                if (!b)
                {
                    D3D11_BLEND_DESC bd = {};
                    auto& rt = bd.RenderTarget[0];
                    rt.BlendEnable = s.blend;
                    rt.SrcBlend = BlendFactor(s.srcRgb);
                    rt.DestBlend = BlendFactor(s.dstRgb);
                    rt.BlendOp = BlendOp(s.blendEqRgb);
                    rt.SrcBlendAlpha = AlphaBlendFactor(s.srcAlpha);
                    rt.DestBlendAlpha = AlphaBlendFactor(s.dstAlpha);
                    rt.BlendOpAlpha = BlendOp(s.blendEqAlpha);
                    rt.RenderTargetWriteMask = UINT8(s.colorMask);
                    g_device->CreateBlendState(&bd, cache[k].put());
                    b = cache[k].get();
                }
                float factor[4] = { 0, 0, 0, 0 };
                g_context->OMSetBlendState(b, factor, 0xFFFFFFFF);
            }
            {
                uint64_t key = uint64_t(s.depthTest) | uint64_t(s.depthMask) << 1 | uint64_t(s.depthFunc & 0xF) << 2 |
                               uint64_t(s.stencil) << 6 | uint64_t(s.stencilFunc & 0xF) << 7 | uint64_t(s.stencilReadMask & 0xFF) << 11 |
                               uint64_t(s.stencilWriteMask & 0xFF) << 19 | uint64_t(s.stencilFail & 0xFFFF) << 27 |
                               uint64_t(s.stencilPass & 0xFFFF) << 43;
                static std::map<uint64_t, Com<ID3D11DepthStencilState>> cache;
                ID3D11DepthStencilState* d = Cached(cache, key);
                if (!d)
                {
                    D3D11_DEPTH_STENCIL_DESC dd = {};
                    dd.DepthEnable = s.depthTest;
                    dd.DepthWriteMask = s.depthMask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
                    dd.DepthFunc = Compare(s.depthFunc);
                    dd.StencilEnable = s.stencil;
                    dd.StencilReadMask = UINT8(s.stencilReadMask);
                    dd.StencilWriteMask = UINT8(s.stencilWriteMask);
                    D3D11_DEPTH_STENCILOP_DESC op = { StencilOp(s.stencilFail), StencilOp(s.stencilDepthFail), StencilOp(s.stencilPass), Compare(s.stencilFunc) };
                    dd.FrontFace = dd.BackFace = op;
                    g_device->CreateDepthStencilState(&dd, cache[key].put());
                    d = cache[key].get();
                }
                g_context->OMSetDepthStencilState(d, UINT(s.stencilRef));
            }
            {
                // Effects are drawn into Unity's targets with the matrices the game gives
                // (D3D conventions): winding as GL sees it can be flipped, so culling
                // follows GL unless GOSR_SPFX_NOCULL is set.
                static const bool noCull = GetEnvironmentVariableW(L"GOSR_SPFX_NOCULL", nullptr, 0) != 0;
                D3D11_CULL_MODE cull = D3D11_CULL_NONE;
                if (s.cull && !noCull && !(coinDraw143 && GetEnvironmentVariableW(L"GOSR_COIN_NOCULL", nullptr, 0)))
                    cull = s.cullFace == 0x0404 ? D3D11_CULL_FRONT : s.cullFace == 0x0405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
                bool ccw = (s.frontFace == 0x0901) != g_flipCameraY;
                uint32_t key = uint32_t(cull) | (ccw ? 16 : 0);
                static std::map<uint32_t, Com<ID3D11RasterizerState>> cache;
                ID3D11RasterizerState* r = Cached(cache, key);
                if (!r)
                {
                    D3D11_RASTERIZER_DESC rd = {};
                    rd.FillMode = D3D11_FILL_SOLID;
                    rd.CullMode = cull;
                    rd.FrontCounterClockwise = ccw;
                    rd.DepthClipEnable = TRUE;
                    g_device->CreateRasterizerState(&rd, cache[key].put());
                    r = cache[key].get();
                }
                g_context->RSSetState(r);
            }
        }

        uint64_t g_draws = 0;
    }

    // ---------------------------------------------------------------- recording

    void SetTextureImage(Texture& t, int32_t level, uint32_t internalFormat, int32_t width, int32_t height,
                         uint32_t format, uint32_t type, const void* pixels, int32_t compressedSize)
    {
        if (level == 0)
        {
            t.internalFormat = internalFormat;
            t.width = uint32_t(width);
            t.height = uint32_t(height);
            t.compressed = compressedSize > 0;
            t.screenCopy = false;
            t.levels.clear();
        }
        if (t.levels.size() <= size_t(level))
            t.levels.resize(size_t(level) + 1);
        int channels = 4;
        if (!t.compressed)
            ExpandedFormat(format ? format : internalFormat, channels);
        size_t bytes = compressedSize > 0 ? size_t(compressedSize) : size_t(width) * size_t(height) * size_t(channels);
        if (!t.compressed && type != 0x1401)  // only UNSIGNED_BYTE uploads are expected
        {
            static int logged = 0;
            if (logged++ < 10)
                Log("teximage type 0x%x not supported", type);
        }
        t.levels[size_t(level)].assign(bytes, 0);
        if (pixels)
            memcpy(t.levels[size_t(level)].data(), pixels, bytes);
        t.version++;
    }

    void LinkProgramImpl(uint32_t name, Program& p);

    // GOSR_SPFX_LINK_STATS: how long linking takes and how fast programs come
    void LinkProgram(uint32_t name, Program& p)
    {
        static const bool stats = GetEnvironmentVariableW(L"GOSR_SPFX_LINK_STATS", nullptr, 0) != 0;
        if (!stats)
            return LinkProgramImpl(name, p);
        static LARGE_INTEGER freq, first;
        static double total = 0;
        static int count = 0;
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        if (!count++)
        {
            QueryPerformanceFrequency(&freq);
            first = a;
        }
        LinkProgramImpl(name, p);
        QueryPerformanceCounter(&b);
        total += double(b.QuadPart - a.QuadPart) / double(freq.QuadPart);
        if (count % 100 == 0)
            Log("link stats: %d programs, %.0f ms linking, %.0f ms since the first", count, total * 1000.0,
                double(b.QuadPart - first.QuadPart) * 1000.0 / double(freq.QuadPart));
    }

    void LinkProgramImpl(uint32_t name, Program& p)
    {
        static const std::regex attr(R"((?:attribute|ATTRIBUTE)\s+(?:(?:highp|mediump|lowp)\s+)?\w+\s+(\w+)\s*;)");
        static const std::regex decl(R"(uniform\s+(?:(?:highp|mediump|lowp)\s+)?(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;)");
        delete static_cast<GpuProgram*>(p.gpu);
        p.gpu = nullptr;
        p.build.reset();
        p.attribs.clear();
        int32_t loc = 0;
        for (auto it = std::sregex_iterator(p.vertexSource.begin(), p.vertexSource.end(), attr); it != std::sregex_iterator(); ++it)
            p.attribs[(*it)[1]] = loc++;
        p.uniforms.clear();
        p.uniformIndex.clear();
        p.locationElement.clear();
        p.locationUniform.clear();
        for (const std::string* src : { &p.vertexSource, &p.fragmentSource })
        {
            for (auto it = std::sregex_iterator(src->begin(), src->end(), decl); it != std::sregex_iterator(); ++it)
            {
                std::string type = (*it)[1], uname = (*it)[2];
                if (p.uniformIndex.count(uname))
                    continue;  // declared by both stages
                Uniform u;
                u.name = uname;
                u.type = type.rfind("sampler", 0) == 0 ? 1 : 0;
                u.cube = type == "samplerCube";
                u.arraySize = (*it)[3].matched ? std::stoi((*it)[3]) : 1;
                u.floats = type == "mat4" ? 16 : 4;
                u.values.assign(size_t(u.arraySize) * u.floats, 0.0f);
                uint32_t index = uint32_t(p.uniforms.size());
                p.uniforms.push_back(u);
                for (int32_t i = 0; i < u.arraySize; i++)
                {
                    int32_t location = int32_t(p.locationUniform.size());
                    p.locationUniform.push_back(index);
                    p.locationElement.push_back(i);
                    if (i == 0)
                        p.uniformIndex[uname] = location;
                    p.uniformIndex[uname + "[" + std::to_string(i) + "]"] = location;
                }
            }
        }
        p.linked = true;
        p.build = RequestBuild(name, p);  // start compiling now, ahead of the first draw
        static int dumps = 0;
        static const bool dump = GetEnvironmentVariableW(L"GOSR_SPFX_DUMP_SHADERS", nullptr, 0) != 0;
        if (dump && dumps++ < 400)
        {
            CreateDirectoryA("gosr_spfx_shaders", nullptr);
            char path[128];
            snprintf(path, sizeof path, "gosr_spfx_shaders/p%04u.vs", name);
            if (FILE* f = fopen(path, "w")) { fputs(p.vertexSource.c_str(), f); fclose(f); }
            snprintf(path, sizeof path, "gosr_spfx_shaders/p%04u.fs", name);
            if (FILE* f = fopen(path, "w")) { fputs(p.fragmentSource.c_str(), f); fclose(f); }
        }
    }

    void SetUniform(Program& p, int32_t location, const void* data, int32_t bytes, bool isInt)
    {
        if (location < 0 || size_t(location) >= p.locationUniform.size())
            return;
        Uniform& u = p.uniforms[p.locationUniform[size_t(location)]];
        int32_t element = p.locationElement[size_t(location)];
        if (isInt)
        {
            int32_t v = int32_t(*static_cast<const uint64_t*>(data));
            if (u.type == 1)
                u.samplerUnit = v;
            else
                u.values[size_t(element) * u.floats] = float(v);
            return;
        }
        size_t offset = size_t(element) * u.floats;
        size_t floats = std::min(size_t(bytes) / 4, u.values.size() - offset);
        memcpy(u.values.data() + offset, data, floats * 4);
    }

    void SetUniformMatrix(Program& p, int32_t location, const float* data, int32_t count, bool transpose)
    {
        if (location < 0 || size_t(location) >= p.locationUniform.size())
            return;
        Uniform& u = p.uniforms[p.locationUniform[size_t(location)]];
        size_t offset = size_t(p.locationElement[size_t(location)]) * u.floats;
        for (int32_t m = 0; m < count && offset + 16 <= u.values.size(); m++, offset += 16)
            for (int i = 0; i < 16; i++)
                u.values[offset + size_t(i)] = transpose ? data[m * 16 + (i % 4) * 4 + i / 4] : data[m * 16 + i];
    }

    void OnDelete(uint32_t name)
    {
        g_buffers.erase(name);
    }

    // glCopyTexImage2D/glCopyTexSubImage2D from the bound framebuffer: copy Unity's
    // current render target into the texture (distortion effects sample it)
    void CopyFramebufferTo(Texture& t)
    {
        if (!EnsureDevice())
            return;
        Com<ID3D11RenderTargetView> rtv;
        g_context->OMGetRenderTargets(1, rtv.put(), nullptr);
        if (!rtv)
            return;
        Com<ID3D11Resource> res;
        rtv->GetResource(res.put());
        Com<ID3D11Texture2D> src;
        if (FAILED(res->QueryInterface(IID_ID3D11Texture2D, reinterpret_cast<void**>(src.put()))))
            return;
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        if (!t.gpu)
            t.gpu = new GpuTexture;
        auto* g = static_cast<GpuTexture*>(t.gpu);
        D3D11_TEXTURE2D_DESC td = {};
        if (g->texture)
            g->texture->GetDesc(&td);
        if (!g->texture || td.Width != sd.Width || td.Height != sd.Height || td.Format != sd.Format)
        {
            td = sd;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.SampleDesc = { 1, 0 };
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = 0;
            td.MiscFlags = 0;
            if (FAILED(g_device->CreateTexture2D(&td, nullptr, g->texture.put())) ||
                FAILED(g_device->CreateShaderResourceView(g->texture.get(), nullptr, g->srv.put())))
                return;
        }
        if (sd.SampleDesc.Count > 1)
            g_context->ResolveSubresource(g->texture.get(), 0, src.get(), 0, sd.Format);
        else
            g_context->CopySubresourceRegion(g->texture.get(), 0, 0, 0, 0, src.get(), 0, nullptr);
        t.screenCopy = true;
        t.width = sd.Width;
        t.height = sd.Height;
    }

    // Unity's native texture pointers (ID3D11Resource*) as GL texture names
    void* NativeTextureName(void* texture)
    {
        if (!texture)
            return nullptr;
        State& st = GetState();
        std::lock_guard<std::recursive_mutex> guard(st.lock);
        static std::unordered_map<void*, uint32_t> names;
        uint32_t& name = names[texture];
        if (!name || !st.textures.count(name))
        {
            name = st.nextName++;
            Texture& t = st.textures[name];
            t.external = texture;
            static_cast<IUnknown*>(texture)->AddRef();  // Unity's textures are kept for the effect's lifetime
        }
        return reinterpret_cast<void*>(uintptr_t(name));
    }

    void* const* NativeTextureNames(void* const* info, int32_t count)
    {
        thread_local std::vector<void*> copy;
        if (!info || count <= 0)
            return info;
        copy.assign(info, info + count);
        for (void*& p : copy)
            p = NativeTextureName(p);
        return copy.data();
    }

    // ------------------------------------------------------------ camera flip
    // The game asks Unity for the texture version of the projection matrix
    // (y flipped) only when the camera has a targetTexture or renders deferred
    // (SPFXSystem.SetupRendering). On D3D11 a camera with image effects also
    // draws into an intermediate texture, and then effects came out upside
    // down (results screen). The matrix is set per camera before the frame;
    // when it is drawn, the bound target tells which convention is right.
    namespace
    {
        constexpr size_t kDrawParameterSize = 0x13c;  // SPFXCore.DRAWPARAMETER
        constexpr size_t kProjection = 0x60;          // MATRIX4x4 ProjectionMatrix, column-major
        std::mutex g_drawParameterLock;
        std::unordered_map<int32_t, std::array<uint8_t, kDrawParameterSize>> g_drawParameters;
        thread_local bool g_replaying = false;
    }

    // gosr.ini [effects] flip_fix=1 turns the experimental correction on (only logged otherwise)
    static bool FlipEnabled()
    {
        static const bool on = [] {
            wchar_t path[MAX_PATH * 4];
            DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
            std::wstring ini(path, len);
            ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"gosr.ini";
            return GetPrivateProfileIntW(L"effects", L"flip_fix", 0, ini.c_str()) != 0;
        }();
        return on;
    }

    void NoteDrawParameter(int32_t camera, const void* parameter)
    {
        if (!parameter || g_replaying)
            return;
        std::lock_guard<std::mutex> lock(g_drawParameterLock);
        if (g_drawParameters.size() > 256)
            g_drawParameters.clear();
        memcpy(g_drawParameters[camera].data(), parameter, kDrawParameterSize);
    }

    // The bound target is the swap chain's back buffer (not one of Unity's
    // render textures, which are all shader resources)
    static bool DrawingIntoBackBuffer(std::string& what)
    {
        Com<ID3D11RenderTargetView> rtv;
        g_context->OMGetRenderTargets(1, rtv.put(), nullptr);
        if (!rtv)
        {
            what = "no target";
            return true;
        }
        Com<ID3D11Resource> res;
        rtv->GetResource(res.put());
        Com<IDXGIResource> dxgi;
        DXGI_USAGE usage = 0;
        if (SUCCEEDED(res->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgi.put()))))
            dxgi->GetUsage(&usage);
        Com<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC td = {};
        if (SUCCEEDED(res->QueryInterface(IID_ID3D11Texture2D, reinterpret_cast<void**>(tex.put()))))
            tex->GetDesc(&td);
        char buf[128];
        snprintf(buf, sizeof buf, "%ux%u format %u bind 0x%x usage 0x%x", td.Width, td.Height, unsigned(td.Format),
                 td.BindFlags, unsigned(usage));
        what = buf;
        return (usage & DXGI_USAGE_BACK_BUFFER) || !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE);
    }

    void BeginCameraDraw(int32_t camera)
    {
        g_flipCameraY = false;
        if (!EnsureDevice())
            return;
        std::array<uint8_t, kDrawParameterSize> p;
        {
            std::lock_guard<std::mutex> lock(g_drawParameterLock);
            auto it = g_drawParameters.find(camera);
            if (it == g_drawParameters.end())
                return;
            p = it->second;
        }
        float* m = reinterpret_cast<float*>(p.data() + kProjection);
        bool flippedMatrix = m[5] < 0;  // m11
        std::string what;
        bool intoTexture = !DrawingIntoBackBuffer(what);
        {
            // every distinct case once in gosr_spfx.log
            static std::mutex lock;
            static std::vector<std::string> seen;
            // what tells the cameras apart (SPFXCore.DRAWPARAMETER)
            auto i32 = [&](size_t offset) { int32_t v; memcpy(&v, p.data() + offset, 4); return v; };
            auto i64 = [&](size_t offset) { int64_t v; memcpy(&v, p.data() + offset, 8); return v; };
            char cam[200];
            snprintf(cam, sizeof cam, ", %s, mask %08x, layers %08x, flags %x, %dx%d, rt %d, depth %d, m11 %.3f m13 %.3f",
                     m[15] == 1.0f ? "ortho" : "perspective", uint32_t(i32(0x124)), uint32_t(i32(0x128)), uint32_t(i32(0x138)),
                     i32(0x130), i32(0x134), i64(0x0) != 0, i64(0x8) != 0, double(m[5]), double(m[13]));
            std::string line = std::string(intoTexture ? "texture " : "back buffer ") + what +
                               (flippedMatrix ? ", texture matrix" : ", screen matrix") + cam;
            std::lock_guard<std::mutex> g(lock);
            if (seen.size() < 64 && std::find(seen.begin(), seen.end(), line) == seen.end())
            {
                seen.push_back(line);
                Log("camera target: %s%s", line.c_str(), flippedMatrix != intoTexture && FlipEnabled() ? " (flipped)" : "");
            }
        }
        if (flippedMatrix == intoTexture || !FlipEnabled())
            return;
        for (int i : { 1, 5, 9, 13 })  // row 1
            m[i] = -m[i];
        g_replaying = true;
        SPFX_PLUGIN_SetDrawParameter(camera, p.data());
        g_replaying = false;
        g_flipCameraY = true;
    }

    void EndCameraDraw() { g_flipCameraY = false; }

    // ---------------------------------------------------------------- drawing

    bool WidenByteIndices143(const std::vector<uint8_t>& source, uint64_t offset, size_t count, std::vector<uint16_t>& output)
    {
        if (offset > source.size() || count > source.size() - size_t(offset) || count > UINT32_MAX / sizeof(uint16_t) || !count) return false;
        output.assign(source.begin() + size_t(offset), source.begin() + size_t(offset) + count);
        return true;
    }

    void Draw(uint32_t mode, int32_t first, int32_t count, uint32_t indexType, uint64_t indices)
    {
        State& s = GetState();
        std::lock_guard<std::recursive_mutex> guard(s.lock);
        if (count <= 0 || !EnsureDevice())
            return;
        auto pit = s.programs.find(s.currentProgram);
        if (pit == s.programs.end())
            return;
        Program& p = pit->second;
        GpuProgram* g = Prepare(s.currentProgram, p);
        if (!g)
            return;

        const bool coinDraw143 = CoinDraw143(p, s);
        if (coinDraw143)
        {
            static std::map<uint32_t, int> snapshots;
            if (snapshots.size() < 64 && snapshots[s.currentProgram]++ < 2)
            {
                Log("COIN DRAW prog %u mode %x count %d index %x cull %d face %x front %x depth %d func %x write %d blend %d src %x dst %x mask %x", s.currentProgram, mode, count, indexType, int(s.cull), s.cullFace, s.frontFace, int(s.depthTest), s.depthFunc, int(s.depthMask), int(s.blend), s.srcRgb, s.dstRgb, s.colorMask);
                CreateDirectoryA("gosr_coin_draw", nullptr);
                char path[160];
                snprintf(path, sizeof path, "gosr_coin_draw/p%u.vs", s.currentProgram);
                if (FILE* file = fopen(path, "w")) { fputs(p.vertexSource.c_str(), file); fclose(file); }
                snprintf(path, sizeof path, "gosr_coin_draw/p%u.fs", s.currentProgram);
                if (FILE* file = fopen(path, "w")) { fputs(p.fragmentSource.c_str(), file); fclose(file); }
                snprintf(path, sizeof path, "gosr_coin_draw/p%u-state.txt", s.currentProgram);
                if (FILE* file = fopen(path, "w"))
                {
                    for (size_t i = 0; i < p.uniforms.size(); ++i)
                    {
                        const auto& u = p.uniforms[i]; const auto& slot = g->slots[i];
                        fprintf(file, "%s vs=%d ps=%d floats=%d array=%d sampler=%d", u.name.c_str(), slot.vsOffset, slot.psOffset, u.floats, u.arraySize, u.samplerUnit);
                        for (size_t j = 0; j < u.values.size() && j < 128; ++j) fprintf(file, " %g", double(u.values[j]));
                        fputc('\n', file);
                    }
                    auto& vao143 = s.vao();
                    for (size_t i = 0; i < g->attributes.size() && i < kMaxAttribs; ++i)
                    {
                        const auto& a = vao143.attribs[i];
                        fprintf(file, "attrib %zu %s enabled=%d buffer=%u size=%d type=%x stride=%d offset=%llu\n", i, g->attributes[i].c_str(), int(a.enabled), a.buffer, a.size, a.type, a.stride, static_cast<unsigned long long>(a.offset));
                        auto it = s.buffers.find(a.buffer);
                        if (it != s.buffers.end())
                        {
                            unsigned stride = a.stride ? unsigned(a.stride) : TypeSize(a.type) * unsigned(a.size);
                            for (unsigned v = 0; v < 6 && a.offset + size_t(v)*stride + TypeSize(a.type)*unsigned(a.size) <= it->second.data.size(); ++v)
                            {
                                fprintf(file, "  vertex %u", v);
                                for (int c = 0; c < a.size; ++c) fprintf(file, " %g", double(ReadComponent(it->second.data.data()+a.offset+size_t(v)*stride+c*TypeSize(a.type), a.type, a.normalized)));
                                fputc('\n', file);
                            }
                        }
                    }
                    fclose(file);
                }
            }
        }

        // constant buffers and textures
        ID3D11ShaderResourceView* vsSrv[16] = {}, * psSrv[16] = {};
        ID3D11SamplerState* vsSmp[16] = {}, * psSmp[16] = {};
        UINT vsSrvCount = 0, psSrvCount = 0, vsSmpCount = 0, psSmpCount = 0;
        for (size_t i = 0; i < p.uniforms.size(); i++)
        {
            const Uniform& u = p.uniforms[i];
            const UniformSlot& slot = g->slots[i];
            if (u.type == 0)
            {
                size_t bytes = u.values.size() * 4;
                if (slot.vsOffset >= 0 && slot.vsOffset + bytes <= g->vsData.size())
                    memcpy(g->vsData.data() + slot.vsOffset, u.values.data(), bytes);
                if (slot.psOffset >= 0 && slot.psOffset + bytes <= g->psData.size())
                    memcpy(g->psData.data() + slot.psOffset, u.values.data(), bytes);
                continue;
            }
            if (slot.vsTexture < 0 && slot.psTexture < 0)
                continue;
            uint32_t unit = uint32_t(u.samplerUnit) < kMaxUnits ? uint32_t(u.samplerUnit) : 0;
            const TextureUnit& tu = s.unit[unit];
            uint32_t texName = u.cube ? tu.textureCube : tu.texture2d;
            ID3D11ShaderResourceView* srv = u.cube ? nullptr : TextureView(texName);
            const std::map<uint32_t, float>* params = nullptr;
            static const std::map<uint32_t, float> none;
            bool mip = false;
            if (auto t = s.textures.find(texName); t != s.textures.end())
            {
                params = &t->second.params;
                mip = t->second.levels.size() > 1;
            }
            if (tu.sampler)
                params = &s.samplers[tu.sampler].params;
            ID3D11SamplerState* smp = SamplerFor(params ? *params : none, mip);
            static const bool trace = GetEnvironmentVariableW(L"GOSR_SPFX_TRACE_DRAWS", nullptr, 0) != 0;
            static std::vector<std::string> traced143;
            std::string key143 = std::to_string(s.currentProgram) + ":" + u.name + ":" + std::to_string(texName);
            if (trace && traced143.size() < 512 && std::find(traced143.begin(), traced143.end(), key143) == traced143.end())
            {
                traced143.push_back(key143);
                auto t = s.textures.find(texName);
                Log("  draw %llu prog %u %s unit %u tex %u %s %ux%u fmt 0x%x levels %zu srv %p", static_cast<unsigned long long>(g_draws),
                    s.currentProgram, u.name.c_str(), unit, texName, t == s.textures.end() ? "MISSING" : "ok",
                    t == s.textures.end() ? 0 : t->second.width, t == s.textures.end() ? 0 : t->second.height,
                    t == s.textures.end() ? 0 : t->second.internalFormat, t == s.textures.end() ? 0 : t->second.levels.size(),
                    static_cast<void*>(srv));
            }
            if (slot.psTexture >= 0 && slot.psTexture < 16) { psSrv[slot.psTexture] = srv; psSrvCount = std::max(psSrvCount, UINT(slot.psTexture + 1)); }
            if (slot.psSampler >= 0 && slot.psSampler < 16) { psSmp[slot.psSampler] = smp; psSmpCount = std::max(psSmpCount, UINT(slot.psSampler + 1)); }
            if (slot.vsTexture >= 0 && slot.vsTexture < 16) { vsSrv[slot.vsTexture] = srv; vsSrvCount = std::max(vsSrvCount, UINT(slot.vsTexture + 1)); }
            if (slot.vsSampler >= 0 && slot.vsSampler < 16) { vsSmp[slot.vsSampler] = smp; vsSmpCount = std::max(vsSmpCount, UINT(slot.vsSampler + 1)); }
        }
        if (g->vsCb) Upload(g->vsCb.get(), g->vsData);
        if (g->psCb) Upload(g->psCb.get(), g->psData);

        // indices (read on the CPU when vertex data must be converted)
        VertexArray& vao = s.vao();
        const Buffer* indexBuffer = nullptr;
        if (indexType)
        {
            auto it = s.buffers.find(vao.elementBuffer);
            if (it == s.buffers.end())
                return;  // client-side index arrays are not used by SPFX
            indexBuffer = &it->second;
        }
        uint32_t indexSize = indexType == 0x1405 ? 4 : indexType == 0x1401 ? 1 : 2;
        uint32_t maxVertex = uint32_t(first + count);
        auto vertexRange = [&]() {
            if (!indexBuffer)
                return;
            maxVertex = 0;
            const uint8_t* idx = indexBuffer->data.data() + indices;
            for (int32_t i = 0; i < count && indices + size_t(i + 1) * indexSize <= indexBuffer->data.size(); i++)
            {
                uint32_t v = indexSize == 4 ? reinterpret_cast<const uint32_t*>(idx)[i] : indexSize == 2 ? reinterpret_cast<const uint16_t*>(idx)[i] : idx[i];
                maxVertex = std::max(maxVertex, v + 1);
            }
        };
        bool rangeKnown = false;

        // vertex inputs: TEXCOORD<location>, one input slot per attribute
        D3D11_INPUT_ELEMENT_DESC elements[kMaxAttribs] = {};
        ID3D11Buffer* vbs[kMaxAttribs] = {};
        UINT strides[kMaxAttribs] = {}, offsets[kMaxAttribs] = {};
        UINT n = 0;
        std::string layoutKey;
        if (!g_defaultAttrib)
        {
            float def[4] = { 0, 0, 0, 1 };
            D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
            D3D11_SUBRESOURCE_DATA sd = { def, 0, 0 };
            g_device->CreateBuffer(&bd, &sd, g_defaultAttrib.put());
        }
        for (UINT loc = 0; loc < g->attributes.size() && loc < kMaxAttribs; loc++)
        {
            const Attrib& a = vao.attribs[loc];
            D3D11_INPUT_ELEMENT_DESC& e = elements[n];
            e.SemanticName = "TEXCOORD";
            e.SemanticIndex = loc;
            e.InputSlot = n;
            e.AlignedByteOffset = 0;
            e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            if (!a.enabled)
            {
                e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                vbs[n] = g_defaultAttrib.get();
                strides[n] = 0;
            }
            else
            {
                DXGI_FORMAT direct = DirectFormat(a);
                uint32_t elemSize = TypeSize(a.type) * uint32_t(a.size);
                uint32_t stride = a.stride ? uint32_t(a.stride) : elemSize;
                const uint8_t* base = nullptr;
                size_t available = 0;
                if (a.buffer)
                {
                    auto b = s.buffers.find(a.buffer);
                    if (b == s.buffers.end())
                        return;
                    base = b->second.data.data();
                    available = b->second.data.size();
                }
                if (direct != DXGI_FORMAT_UNKNOWN && a.buffer)
                {
                    e.Format = direct;
                    vbs[n] = BufferFor(a.buffer, D3D11_BIND_VERTEX_BUFFER);
                    strides[n] = stride;
                    offsets[n] = UINT(a.offset);
                }
                else
                {
                    // convert to float4 on the CPU
                    if (!rangeKnown)
                    {
                        vertexRange();
                        rangeKnown = true;
                    }
                    if (!a.buffer)
                    {
                        base = reinterpret_cast<const uint8_t*>(a.offset);
                        available = size_t(maxVertex) * stride;
                    }
                    else
                        base += a.offset;
                    std::vector<float> conv(size_t(maxVertex) * 4, 0.0f);
                    for (uint32_t v = 0; v < maxVertex; v++)
                    {
                        float* d = &conv[size_t(v) * 4];
                        d[3] = 1.0f;
                        const uint8_t* src = base + size_t(v) * stride;
                        if (size_t(src - (a.buffer ? base - a.offset : base)) + elemSize > available + (a.buffer ? 0 : 0))
                            break;
                        for (int c = 0; c < a.size; c++)
                            d[c] = ReadComponent(src + size_t(c) * TypeSize(a.type), a.type, a.normalized);
                    }
                    e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    vbs[n] = ScratchBuffer(loc, conv.data(), UINT(conv.size() * 4));
                    strides[n] = 16;
                }
                if (!vbs[n])
                    return;
            }
            layoutKey += std::to_string(loc) + ":" + std::to_string(int(e.Format)) + ";";
            n++;
        }
        static std::map<std::pair<ID3D11VertexShader*, std::string>, Com<ID3D11InputLayout>> layouts;
        auto lk = std::make_pair(g->vs.get(), layoutKey);
        ID3D11InputLayout* layout = Cached(layouts, lk);
        if (!layout)
        {
            HRESULT hr = g_device->CreateInputLayout(elements, n, g->vsCode->GetBufferPointer(), g->vsCode->GetBufferSize(), layouts[lk].put());
            layout = layouts[lk].get();
            if (FAILED(hr) || !layout)
            {
                static int logged = 0;
                if (logged++ < 10)
                    Log("input layout for program %u failed (0x%08lx): %s", s.currentProgram, hr, layoutKey.c_str());
                return;
            }
        }

        D3D11_PRIMITIVE_TOPOLOGY topology;
        switch (mode)
        {
        case 0: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case 1: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case 3: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        case 5: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        default:
        {
            static int logged = 0;
            if (logged++ < 5)
                Log("primitive mode 0x%x not supported", mode);
            return;
        }
        }

        g_context->IASetInputLayout(layout);
        g_context->IASetPrimitiveTopology(topology);
        g_context->IASetVertexBuffers(0, n, vbs, strides, offsets);
        g_context->VSSetShader(g->vs.get(), nullptr, 0);
        g_context->PSSetShader(g->ps.get(), nullptr, 0);
        g_context->GSSetShader(nullptr, nullptr, 0);
        g_context->HSSetShader(nullptr, nullptr, 0);
        g_context->DSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* vcb = g->vsCb.get();
        ID3D11Buffer* pcb = g->psCb.get();
        g_context->VSSetConstantBuffers(0, 1, &vcb);
        g_context->PSSetConstantBuffers(0, 1, &pcb);
        if (vsSrvCount) g_context->VSSetShaderResources(0, vsSrvCount, vsSrv);
        if (vsSmpCount) g_context->VSSetSamplers(0, vsSmpCount, vsSmp);
        if (psSrvCount) g_context->PSSetShaderResources(0, psSrvCount, psSrv);
        if (psSmpCount) g_context->PSSetSamplers(0, psSmpCount, psSmp);
        ApplyFixedFunction(s, coinDraw143);
        if (indexBuffer)
        {
            Com<ID3D11Buffer> widened;
            ID3D11Buffer* ib = nullptr;
            UINT startIndex = UINT(indices / indexSize);
            if (indexSize == 1)
            {
                std::vector<uint16_t> converted;
                if (!WidenByteIndices143(indexBuffer->data, indices, size_t(count), converted)) return;
                D3D11_BUFFER_DESC desc = {};
                desc.ByteWidth = UINT(converted.size() * sizeof(uint16_t));
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
                D3D11_SUBRESOURCE_DATA initial = { converted.data(), 0, 0 };
                if (FAILED(g_device->CreateBuffer(&desc, &initial, widened.put()))) return;
                ib = widened.get();
                startIndex = 0;
                static bool logged = false;
                if (!logged) { Log("8-bit GL indices widened to 16-bit: program %u, count %d", s.currentProgram, count); logged = true; }
            }
            else ib = BufferFor(vao.elementBuffer, D3D11_BIND_INDEX_BUFFER);
            if (!ib) return;
            g_context->IASetIndexBuffer(ib, indexSize == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
            g_context->DrawIndexed(UINT(count), startIndex, 0);
        }
        else
            g_context->Draw(UINT(count), UINT(first));
        if (g_draws++ == 0)
            Log("first effect draw (program %u, %d vertices)", s.currentProgram, count);
    }
}
