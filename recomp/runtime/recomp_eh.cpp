#include "recomp_eh.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

#include "recomp_bridge.h"

namespace recomp
{
    namespace
    {
        // Typeinfo of Il2CppExceptionWrapper in the macOS v1.4.3 arm64 image.
        constexpr uint64_t kWrapperTypeInfo = 0x3aec148;

        // C++ ABI stubs the game code calls.
        constexpr uint64_t kUnwindResume = 0x343026c;
        constexpr uint64_t kTerminate = 0x3430434;
        constexpr uint64_t kAllocateException = 0x343047c;
        constexpr uint64_t kBeginCatch = 0x3430494;
        constexpr uint64_t kEndCatch = 0x34304a0;
        constexpr uint64_t kFreeException = 0x34304ac;
        constexpr uint64_t kRethrow = 0x34304dc;
        constexpr uint64_t kThrow = 0x34304e8;

        // An exception in flight between a landing pad and __cxa_end_catch or
        // _Unwind_Resume. x0 handed to the game points at `object`, which
        // starts with the Il2CppException* like the original wrapper.
        struct InFlight
        {
            Il2CppException* object = nullptr;
            std::exception_ptr native;
        };

        thread_local std::vector<InFlight*> t_inFlight;

        InFlight* Push()
        {
            auto* e = new InFlight;
            e->native = std::current_exception();
            t_inFlight.push_back(e);
            return e;
        }

        InFlight* FromGuest(uint64_t x0)
        {
            for (auto it = t_inFlight.rbegin(); it != t_inFlight.rend(); ++it)
            {
                if (reinterpret_cast<uint64_t>(&(*it)->object) == x0)
                    return *it;
            }
            return nullptr;
        }

        void Pop(InFlight* e)
        {
            for (auto it = t_inFlight.begin(); it != t_inFlight.end(); ++it)
            {
                if (*it == e)
                {
                    t_inFlight.erase(it);
                    break;
                }
            }
            delete e;
        }

        [[noreturn]] void Fatal(const char* what)
        {
            fprintf(stderr, "recomp: %s\n", what);
            fflush(stderr);
            abort();
        }

        void BeginCatch(Ctx& c)
        {
            // The pointer we handed to the landing pad already points at the
            // caught object.
        }

        void EndCatch(Ctx&)
        {
            if (!t_inFlight.empty())
                Pop(t_inFlight.back());
        }

        void UnwindResume(Ctx& c)
        {
            InFlight* e = FromGuest(c.x[0]);
            if (!e)
                Fatal("_Unwind_Resume with an unknown exception");
            std::exception_ptr native = e->native;
            Pop(e);
            std::rethrow_exception(native);
        }

        void Rethrow(Ctx&)
        {
            if (t_inFlight.empty())
                Fatal("__cxa_rethrow without a caught exception");
            InFlight* e = t_inFlight.back();
            std::exception_ptr native = e->native;
            Pop(e);
            std::rethrow_exception(native);
        }

        void AllocateException(Ctx& c)
        {
            c.x[0] = reinterpret_cast<uint64_t>(calloc(1, c.x[0] < 16 ? 16 : c.x[0]));
        }

        void FreeException(Ctx& c)
        {
            free(reinterpret_cast<void*>(c.x[0]));
        }

        void Throw(Ctx& c)
        {
            void* object = reinterpret_cast<void*>(c.x[0]);
            uint64_t typeInfo = c.x[1];
            if (typeInfo != g_imageBase + kWrapperTypeInfo)
                Fatal("game code threw a C++ exception of an unsupported type");
            Il2CppException* ex = *static_cast<Il2CppException**>(object);
            free(object);
            throw Il2CppExceptionWrapper(ex);
        }

        void Terminate(Ctx&)
        {
            Fatal("std::terminate called in game code");
        }
    }

    void LandingCatch(Ctx& c, const Il2CppExceptionWrapper& e, int selector)
    {
        InFlight* f = Push();
        f->object = e.ex;
        c.x[0] = reinterpret_cast<uint64_t>(&f->object);
        c.x[1] = uint64_t(uint32_t(selector));
    }

    void LandingCatchAll(Ctx& c, int selector)
    {
        InFlight* f = Push();
        c.x[0] = reinterpret_cast<uint64_t>(&f->object);
        c.x[1] = uint64_t(uint32_t(selector));
    }

    void LandingCleanup(Ctx& c)
    {
        InFlight* f = Push();
        c.x[0] = reinterpret_cast<uint64_t>(&f->object);
        c.x[1] = 0;
    }

    // __clang_call_terminate: begin catch, then terminate; never return.
    void ClangCallTerminate143(Ctx& c)
    {
        SetGuestStackPointer(c.sp);
        BeginCatch(c);
        Terminate(c);
    }

    void RegisterExceptionImports()
    {
        RegisterImport(kUnwindResume, UnwindResume);
        RegisterImport(kTerminate, Terminate);
        RegisterImport(kAllocateException, AllocateException);
        RegisterImport(kBeginCatch, BeginCatch);
        RegisterImport(kEndCatch, EndCatch);
        RegisterImport(kFreeException, FreeException);
        RegisterImport(kRethrow, Rethrow);
        RegisterImport(kThrow, Throw);
    }
}
