#include "VapourSynth.h"

#include "expr/internalfilters.h"

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("info.akarin.vsplugin", "akarin", "Akarin's Experimental Filters", VAPOURSYNTH_API_VERSION, 1, plugin);
    exprInitialize(configFunc, registerFunc, plugin);
}
