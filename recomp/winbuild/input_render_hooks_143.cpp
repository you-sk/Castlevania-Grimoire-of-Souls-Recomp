#include <cstdint>
#include "il2cpp-api.h"
#include "recomp_ctx.h"
namespace recomp {
void f_1c0ea94_orig(Ctx&); void f_2f6b350_orig(Ctx&); void f_2f69064_orig(Ctx&);
bool MouseButtonsEnabled();
void HookKeyboardInputText143(Ctx& c) {
    const char* name=nullptr;
    if(MouseButtonsEnabled()) switch(int32_t(uint32_t(c.x[0]))) {
        case 83:name="Mouse L";break; case 82:name="Mouse R";break;
        case 49:name="Mouse M";break; case 47:name="Mouse 4";break; case 48:name="Mouse 5";break;
    }
    if(!name) { f_1c0ea94_orig(c);return; }
    c.x[0]=reinterpret_cast<uint64_t>(il2cpp_string_new(name));
}
static void ApplyMetalReverse143(uint64_t object) {
    if(auto* self=reinterpret_cast<uint8_t*>(object)) {
        self[0x41]=self[0x43];
        self[0x46]=self[0x43];
    }
}
void HookSimpleCameraVRev143(Ctx& c) { uint64_t self=c.x[0];f_2f6b350_orig(c);ApplyMetalReverse143(self); }
// In 1.4.3 the API selector is inlined into OnEnable. OnPreRender reads 0x46.
void HookCommandBufferOnEnable143(Ctx& c) { uint64_t self=c.x[0];f_2f69064_orig(c);ApplyMetalReverse143(self); }
}
