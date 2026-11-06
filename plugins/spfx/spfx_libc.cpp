// C library functions the SPFX plugin imports, called with Apple's arm64
// conventions: arguments in x0-x7 / v0-v7, variadic arguments in 8-byte stack
// slots (a va_list is a pointer to such slots).
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "recomp_bridge.h"
#include "recomp_simd.h"
#include "spfx_host.h"

namespace
{
    using recomp::Ctx;

    void* P(uint64_t v) { return reinterpret_cast<void*>(v); }
    const char* S(uint64_t v) { return reinterpret_cast<const char*>(v); }

    // printf over Apple variadic slots
    std::string Format(const char* fmt, const uint64_t* slots)
    {
        std::string out;
        char spec[64], buf[512];
        for (const char* p = fmt; *p; p++)
        {
            if (*p != '%')
            {
                out += *p;
                continue;
            }
            if (p[1] == '%')
            {
                out += '%';
                p++;
                continue;
            }
            size_t n = 0;
            spec[n++] = '%';
            p++;
            while (*p && strchr("-+ #0123456789.*", *p) && n < sizeof spec - 20)
            {
                if (*p == '*')  // at most 11 characters: n stays below sizeof spec - 8
                {
                    n += snprintf(spec + n, sizeof spec - n, "%d", int(*slots++));
                    p++;
                    continue;
                }
                spec[n++] = *p++;
            }
            bool wide = false;
            while (*p && strchr("hlLqjzt", *p))
            {
                if (*p == 'l' || *p == 'q' || *p == 'j' || *p == 'z' || *p == 't')
                    wide = true;
                p++;
            }
            char conv = *p;
            if (!conv)
                break;
            uint64_t v = *slots++;
            switch (conv)
            {
            case 'd': case 'i':
                memcpy(spec + n, "lld", 4);
                snprintf(buf, sizeof buf, spec, wide ? (long long)int64_t(v) : (long long)int32_t(v));
                break;
            case 'u': case 'x': case 'X': case 'o':
                spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = conv; spec[n] = 0;
                snprintf(buf, sizeof buf, spec, wide ? (unsigned long long)v : (unsigned long long)uint32_t(v));
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                double d;
                memcpy(&d, &v, 8);
                spec[n++] = conv; spec[n] = 0;
                snprintf(buf, sizeof buf, spec, d);
                break;
            }
            case 'c':
                spec[n++] = 'c'; spec[n] = 0;
                snprintf(buf, sizeof buf, spec, int(v));
                break;
            case 's':
                spec[n++] = 's'; spec[n] = 0;
                snprintf(buf, sizeof buf, spec, v ? S(v) : "(null)");
                break;
            case 'p':
                snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
                break;
            default:
                buf[0] = 0;
                break;
            }
            out += buf;
        }
        return out;
    }

    // Apple arm64: every variadic argument is on the stack
    void Sprintf(Ctx& c)
    {
        std::string s = Format(S(c.x[1]), reinterpret_cast<const uint64_t*>(c.sp));
        memcpy(P(c.x[0]), s.c_str(), s.size() + 1);
        c.x[0] = s.size();
    }

    void Vsprintf(Ctx& c)
    {
        std::string s = Format(S(c.x[1]), reinterpret_cast<const uint64_t*>(c.x[2]));
        memcpy(P(c.x[0]), s.c_str(), s.size() + 1);
        c.x[0] = s.size();
    }

    void Malloc(Ctx& c) { c.x[0] = reinterpret_cast<uint64_t>(_aligned_malloc(c.x[0] ? c.x[0] : 1, 16)); }
    void Calloc(Ctx& c)
    {
        size_t n = c.x[0] * c.x[1];
        void* p = _aligned_malloc(n ? n : 1, 16);
        if (p)
            memset(p, 0, n);
        c.x[0] = reinterpret_cast<uint64_t>(p);
    }
    void Free(Ctx& c) { _aligned_free(P(c.x[0])); }
    void Memcpy(Ctx& c) { memcpy(P(c.x[0]), P(c.x[1]), c.x[2]); }
    void Memmove(Ctx& c) { memmove(P(c.x[0]), P(c.x[1]), c.x[2]); }
    void Memset(Ctx& c) { memset(P(c.x[0]), int(c.x[1]), c.x[2]); }
    void Bzero(Ctx& c) { memset(P(c.x[0]), 0, c.x[1]); }
    void MemsetPattern16(Ctx& c)
    {
        auto* d = static_cast<uint8_t*>(P(c.x[0]));
        auto* pat = static_cast<const uint8_t*>(P(c.x[1]));
        for (size_t i = 0; i < c.x[2]; i++)
            d[i] = pat[i % 16];
    }
    void Memcmp(Ctx& c) { c.x[0] = uint64_t(int64_t(memcmp(P(c.x[0]), P(c.x[1]), c.x[2]))); }
    void Strlen(Ctx& c) { c.x[0] = strlen(S(c.x[0])); }
    void Strcmp(Ctx& c) { c.x[0] = uint64_t(int64_t(strcmp(S(c.x[0]), S(c.x[1])))); }
    void Strcpy(Ctx& c) { strcpy(static_cast<char*>(P(c.x[0])), S(c.x[1])); }
    void Strncpy(Ctx& c) { strncpy(static_cast<char*>(P(c.x[0])), S(c.x[1]), c.x[2]); }
    void Strcat(Ctx& c) { strcat(static_cast<char*>(P(c.x[0])), S(c.x[1])); }
    void Strstr(Ctx& c) { c.x[0] = reinterpret_cast<uint64_t>(strstr(S(c.x[0]), S(c.x[1]))); }
    void Time(Ctx& c)
    {
        time_t t = time(nullptr);
        if (c.x[0])
            *static_cast<int64_t*>(P(c.x[0])) = t;
        c.x[0] = uint64_t(t);
    }

    // stdio: FILE* handles are host FILE*s
    void Fopen(Ctx& c) { c.x[0] = reinterpret_cast<uint64_t>(fopen(S(c.x[0]), S(c.x[1]))); }
    void Fclose(Ctx& c) { c.x[0] = uint64_t(int64_t(fclose(static_cast<FILE*>(P(c.x[0]))))); }
    void Fread(Ctx& c) { c.x[0] = fread(P(c.x[0]), c.x[1], c.x[2], static_cast<FILE*>(P(c.x[3]))); }
    void Fseek(Ctx& c) { c.x[0] = uint64_t(int64_t(_fseeki64(static_cast<FILE*>(P(c.x[0])), int64_t(c.x[1]), int(c.x[2])))); }
    void Ftell(Ctx& c) { c.x[0] = uint64_t(_ftelli64(static_cast<FILE*>(P(c.x[0])))); }
    void Perror(Ctx& c) { spfx::Log("perror: %s", c.x[0] ? S(c.x[0]) : ""); }

    template <float (*F)(float)>
    void F1(Ctx& c) { recomp::SetS(c.v[0], F(c.v[0].f32[0])); }
    template <float (*F)(float, float)>
    void F2(Ctx& c) { recomp::SetS(c.v[0], F(c.v[0].f32[0], c.v[1].f32[0])); }
    float Acosf(float x) { return acosf(x); }
    float Asinf(float x) { return asinf(x); }
    float Cbrtf(float x) { return cbrtf(x); }
    float Cosf(float x) { return cosf(x); }
    float Sinf(float x) { return sinf(x); }
    float Exp2f(float x) { return exp2f(x); }
    float Atan2f(float y, float x) { return atan2f(y, x); }
    float Fmodf(float x, float y) { return fmodf(x, y); }

    thread_local int t_errno;
    void Error(Ctx& c) { c.x[0] = reinterpret_cast<uint64_t>(&t_errno); }

    [[noreturn]] void AssertRtn(Ctx& c)
    {
        spfx::Log("assertion failed: %s (%s:%d, %s)", c.x[3] ? S(c.x[3]) : "", c.x[1] ? S(c.x[1]) : "", int(c.x[2]),
                  c.x[0] ? S(c.x[0]) : "");
        abort();
    }
    [[noreturn]] void StackChkFail(Ctx&)
    {
        spfx::Log("stack protector tripped");
        abort();
    }
    void Chkstk(Ctx&) {}

    // Sockets serve SPFX's editor link (Communicator), off in the game.
    void Fail(Ctx& c) { c.x[0] = uint64_t(-1); }
    void Null(Ctx& c) { c.x[0] = 0; }

    // host_info(host, HOST_BASIC_INFO, info, count): CPU counts for the task pipeline
    void HostInfo(Ctx& c)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        auto* info = static_cast<int32_t*>(P(c.x[2]));
        // Each recompiled worker owns a committed 16 MiB guest stack.
        // Bound this plugin's task pool, rather than exhausting a Sandbox.
        DWORD processors = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
        DWORD workers = processors > 8 ? 8 : processors;
        char setting[16] = {};
        DWORD length = GetEnvironmentVariableA("GOSR_SPFX_WORKERS", setting, sizeof(setting));
        if (length > 0 && length < sizeof(setting) && setting[0] >= '0' && setting[0] <= '9')
        {
            char* end = nullptr;
            unsigned long requested = strtoul(setting, &end, 10);
            if (end && *end == 0 && requested >= 1 && requested <= 16)
                workers = requested < processors ? DWORD(requested) : processors;
        }
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            spfx::Log("task pool: %lu workers (host processors %lu)", static_cast<unsigned long>(workers), static_cast<unsigned long>(processors));
        // Darwin HOST_BASIC_INFO is 12 32-bit words, packed to 4 bytes.
        // The task pipeline reads logical_cpu_max at byte 0x24, not max_cpus.
        auto* count = static_cast<uint32_t*>(P(c.x[3]));
        if (c.x[1] != 1 || !info || !count || *count < 12)
        {
            c.x[0] = 4; // KERN_INVALID_ARGUMENT; never write a short buffer.
            return;
        }
        int32_t values[12] = {};
        values[0] = values[1] = int32_t(workers);
        values[3] = 0x0100000c; // CPU_TYPE_ARM64 of the guest plugin.
        values[6] = values[7] = values[8] = values[9] = int32_t(workers);
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
        {
            values[2] = int32_t(memory.ullTotalPhys > 0x7fffffffULL ? 0x7fffffffULL : memory.ullTotalPhys);
            memcpy(&values[10], &memory.ullTotalPhys, sizeof(memory.ullTotalPhys));
        }
        memcpy(info, values, sizeof(values));
        *count = 12;
        c.x[0] = 0;
    }
    void MachHostSelf(Ctx& c) { c.x[0] = 0x203; }

    // pthread_create(thread, attr, start, arg)
    struct ThreadStart
    {
        uint64_t start, arg;
        int index;
    };

    std::atomic<int> g_starting{ 0 };
    HANDLE g_startEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_local bool t_announce = false;  // a new thread not yet through its start
    thread_local int t_index = 0;
    std::atomic<int> g_threadCount{ 0 };

    double NowMs()
    {
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return double(now.QuadPart) * 1000.0 / double(freq.QuadPart);
    }
    double g_createdAt[64];

    DWORD WINAPI ThreadMain(void* p)
    {
        ThreadStart s = *static_cast<ThreadStart*>(p);
        delete static_cast<ThreadStart*>(p);
        t_announce = true;
        t_index = s.index;
        if (t_index < 64)
            spfx::Log("thread %d running %.1f ms after its creation", t_index, NowMs() - g_createdAt[t_index]);
        spfx::CallGuest(s.start, s.arg);
        return 0;
    }

    void PthreadCreate(Ctx& c)
    {
        int index = g_threadCount++;
        if (index < 64)
            g_createdAt[index] = NowMs();
        g_starting++;
        auto* s = new ThreadStart{ c.x[2], c.x[3], index };
        HANDLE h = CreateThread(nullptr, 0, ThreadMain, s, 0, nullptr);
        *static_cast<uint64_t*>(P(c.x[0])) = reinterpret_cast<uint64_t>(h);
        if (!h)
            g_starting--;
        c.x[0] = h ? 0 : 11;  // EAGAIN
    }

    void SchedYield(Ctx& c)
    {
        SwitchToThread();
        c.x[0] = 0;
    }
}

namespace spfx
{
    void NoteThreadStarted()
    {
        if (!t_announce)
            return;
        t_announce = false;
        if (t_index < 64)
            Log("thread %d started %.1f ms after its creation", t_index, NowMs() - g_createdAt[t_index]);
        g_starting--;
        SetEvent(g_startEvent);
    }

    bool WaitForThreadStart(uint32_t ms)
    {
        if (g_starting <= 0)
            return false;
        double start = NowMs();
        if (WaitForSingleObject(g_startEvent, ms) != WAIT_OBJECT_0)
            return true;  // slept the whole time
        // the new thread sets its started flag right after: let it
        while (NowMs() - start < ms && NowMs() - start < 1.0)
            SwitchToThread();
        return true;
    }

    void RegisterLibcImports()
    {
        RegisterHandler("_sprintf", Sprintf);
        RegisterHandler("_vsprintf", Vsprintf);
        RegisterHandler("_malloc", Malloc);
        RegisterHandler("_calloc", Calloc);
        RegisterHandler("_free", Free);
        RegisterHandler("_memcpy", Memcpy);
        RegisterHandler("_memmove", Memmove);
        RegisterHandler("_memset", Memset);
        RegisterHandler("_bzero", Bzero);
        RegisterHandler("_memset_pattern16", MemsetPattern16);
        RegisterHandler("_memcmp", Memcmp);
        RegisterHandler("_strlen", Strlen);
        RegisterHandler("_strcmp", Strcmp);
        RegisterHandler("_strcpy", Strcpy);
        RegisterHandler("_strncpy", Strncpy);
        RegisterHandler("_strcat", Strcat);
        RegisterHandler("_strstr", Strstr);
        RegisterHandler("_time", Time);
        RegisterHandler("_fopen", Fopen);
        RegisterHandler("_fclose", Fclose);
        RegisterHandler("_fread", Fread);
        RegisterHandler("_fseek", Fseek);
        RegisterHandler("_ftell", Ftell);
        RegisterHandler("_perror", Perror);
        RegisterHandler("_acosf", F1<Acosf>);
        RegisterHandler("_asinf", F1<Asinf>);
        RegisterHandler("_cbrtf", F1<Cbrtf>);
        RegisterHandler("_cosf", F1<Cosf>);
        RegisterHandler("_sinf", F1<Sinf>);
        RegisterHandler("_exp2f", F1<Exp2f>);
        RegisterHandler("_atan2f", F2<Atan2f>);
        RegisterHandler("_fmodf", F2<Fmodf>);
        RegisterHandler("___error", Error);
        RegisterHandler("___assert_rtn", AssertRtn);
        RegisterHandler("___stack_chk_fail", StackChkFail);
        RegisterHandler("___chkstk_darwin", Chkstk);
        for (const char* s : { "_socket", "_bind", "_listen", "_accept", "_recv", "_send", "_shutdown", "_close", "_ioctl",
                               "_getpeername", "_gethostname" })
            RegisterHandler(s, Fail);
        for (const char* s : { "_gethostbyname", "_inet_ntoa", "_dispatch_data_create" })
            RegisterHandler(s, Null);
        RegisterHandler("_host_info", HostInfo);
        RegisterHandler("_mach_host_self", MachHostSelf);
        RegisterHandler("_pthread_create", PthreadCreate);
        RegisterHandler("_sched_yield", SchedYield);
    }
}
