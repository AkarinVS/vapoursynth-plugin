#include "VapourSynth.h"

#include "expr/internalfilters.h"
#include "ngx/internalfilters.h"
#include "vfx/internalfilters.h"
#include "banding/internalfilters.h"

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("info.akarin.vsplugin", "akarin", "Akarin's Experimental Filters", VAPOURSYNTH_API_VERSION, 1, plugin);
    exprInitialize(configFunc, registerFunc, plugin);
#ifdef HAVE_NGX
    ngxInitialize(configFunc, registerFunc, plugin);
#endif
#ifdef HAVE_VFX
    vfxInitialize(configFunc, registerFunc, plugin);
#endif
    bandingInitialize(configFunc, registerFunc, plugin);
}
