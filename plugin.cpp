#include <vector>

#include "VapourSynth.h"

#include "plugin.h"
#include "version.h"

#include "expr/internalfilters.h"
#include "ngx/internalfilters.h"
#include "vfx/internalfilters.h"
#include "banding/internalfilters.h"
#include "text/internalfilters.h"

static std::vector<VSPublicFunction> versionFuncs;

void registerVersionFunc(VSPublicFunction f) {
    versionFuncs.push_back(f);
}

void VS_CC versionCreate(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    for (auto f: versionFuncs)
        f(in, out, user_data, core, vsapi);
    vsapi->propSetData(out, "version", VERSION, -1, paAppend);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("info.akarin.vsplugin", "akarin", "Akarin's Experimental Filters", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Version", "", versionCreate, nullptr, plugin);
    exprInitialize(configFunc, registerFunc, plugin);
#ifdef HAVE_NGX
    ngxInitialize(configFunc, registerFunc, plugin);
#endif
#ifdef HAVE_VFX
    vfxInitialize(configFunc, registerFunc, plugin);
#endif
    bandingInitialize(configFunc, registerFunc, plugin);
    textInitialize(configFunc, registerFunc, plugin);
    tmplInitialize(configFunc, registerFunc, plugin);
}
