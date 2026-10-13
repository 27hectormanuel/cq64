// SPDX-License-Identifier: CC0-1.0
// FMOD output selection. FMOD Ex on Android prefers OpenSL ES, which the host does not
// bridge; its Java AudioTrack output (org.fmod.FMODAudioDevice) works through JNI.
#include "abi.h"

namespace {
u32 original(const char *sym) {
    Module *m = loader::find("libfmodex.so");
    u32 a = m ? loader::lookup_in(m, sym) : 0;
    if (!a) fatal("libfmodex.so lacks %s", sym);
    return a;
}

constexpr const char *SET_OUTPUT = "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE";
s32 t_setOutput(u32 self, s32 type) {
    s32 r = (s32)cpu::call32(original(SET_OUTPUT), {self, (u32)type});
    LOGI("FMOD System::setOutput(%d) = %d", type, r);
    if (r) {
        u32 buf = mem::halloc(128);
        cpu::call32(original("_ZN4FMOD6System13getNumPluginsE15FMOD_PLUGINTYPEPi"), {self, 0, buf});
        s32 n = rd<s32>(buf);
        LOGI("FMOD: %d output plugins", n);
        for (s32 i = 0; i < n; i++) {
            cpu::call32(original("_ZN4FMOD6System15getPluginHandleE15FMOD_PLUGINTYPEiPj"), {self, 0, (u32)i, buf});
            u32 h = rd<u32>(buf);
            s32 e = (s32)cpu::call32(original("_ZN4FMOD6System13getPluginInfoEjP15FMOD_PLUGINTYPEPciPj"),
                                     {self, h, buf, buf + 16, 100, buf + 4});
            LOGI("FMOD:   plugin %d handle %u: %s (err %d)", i, h, (const char *)g2h(buf + 16), e);
        }
        mem::hfree(buf);
    }
    return r;
}
THUNK(override:_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE, t_setOutput);

constexpr const char *SYS_INIT = "_ZN4FMOD6System4initEijPv";
s32 t_init(u32 self, s32 maxch, u32 flags, u32 extra) {
    s32 r = (s32)cpu::call32(original(SYS_INIT), {self, (u32)maxch, flags, extra});
    u32 out = mem::halloc(4);
    cpu::call32(original("_ZN4FMOD6System9getOutputEP15FMOD_OUTPUTTYPE"), {self, out});
    LOGI("FMOD System::init(%d, 0x%x) = %d, output type %d", maxch, flags, r, rd<s32>(out));
    mem::hfree(out);
    return r;
}
THUNK(override:_ZN4FMOD6System4initEijPv, t_init);

u32 original_event(const char *sym) {
    Module *m = loader::find("libfmodevent.so");
    u32 a = m ? loader::lookup_in(m, sym) : 0;
    if (!a) fatal("libfmodevent.so lacks %s", sym);
    return a;
}
s32 t_ev_init(u32 self, s32 maxch, u32 flags, u32 extra, u32 evflags) {
    LOGI("FMOD EventSystem::init(%d, 0x%x, 0x%x, 0x%x)", maxch, flags, extra, evflags);
    s32 r = (s32)cpu::call32(original_event("_ZN4FMOD11EventSystem4initEijPvj"), {self, (u32)maxch, flags, extra, evflags});
    LOGI("FMOD EventSystem::init = %d", r);
    return r;
}
THUNK(override:_ZN4FMOD11EventSystem4initEijPvj, t_ev_init);

}  // namespace
