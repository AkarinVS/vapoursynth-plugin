#include <cstdint>
#include <cstdlib>

#include <functional>
#include <iterator>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include <VapourSynth.h>
#include <VSHelper.h>

#include "inja/inja.hpp"

#include "../plugin.h"

static const std::string clipNamePrefix { "src" };

static std::vector<std::string> features = {
    "x.property", "{{N}}",
    clipNamePrefix + "0", clipNamePrefix + "26",
};

namespace {

using json = nlohmann::json;
typedef std::function<bool (const json::json_pointer &)> contains_type;
typedef std::function<const json &(const json::json_pointer &)> get_type;
class data_provider: public inja::json_like {
    const contains_type fcontains;
    const get_type fget;

public:
    data_provider(const contains_type &c, const get_type &g): fcontains(c), fget(g) {}
    virtual ~data_provider() {}

    virtual bool contains(const json::json_pointer &ptr) const override { return fcontains(ptr); }
    virtual const json &operator[](const json::json_pointer &ptr) const override { return fget(ptr); }
};

typedef struct {
    std::vector<VSNodeRef *> nodes;
    const VSVideoInfo *vi;

    std::vector<std::string> text;
    std::vector<std::string> propName;

    inja::Environment env;
    std::vector<inja::Template> tmpl;
} TmplData;

static void VS_CC tmplInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    TmplData *d = static_cast<TmplData *>(*instanceData);
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC tmplGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    TmplData *d = static_cast<TmplData *>(*instanceData);

    if (activationReason == arInitial) {
        for (auto node: d->nodes)
            vsapi->requestFrameFilter(n, node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        std::vector<const VSFrameRef *> srcs;
        const VSFrameRef *src = nullptr;
        std::vector<std::string> out(d->tmpl.size());
        try {
            for (auto node: d->nodes) {
                auto f = vsapi->getFrameFilter(n, node, frameCtx);
                srcs.push_back(f);
            }

            src = srcs[0];
            std::vector<const VSMap *> maps(srcs.size(), nullptr);

            auto extractClipId = [](const std::string &name) -> int {
                if (name.size() == 1)
                    return name[0] >= 'x' ? name[0] - 'x' : name[0] - 'a' + 3;
                int idx = -1;
                try {
                    idx = std::stoi(name.substr(clipNamePrefix.size()));
                } catch (...) {
                    throw std::runtime_error("invalid clip name: " + name);
                }
                return idx;
            };
            auto extractIndex = [](const std::string &str) -> int {
                int idx = -1;
                try {
                    idx = std::stoi(str);
                } catch (...) {
                    throw std::runtime_error("invalid array index: " + str);
                }
                return idx;
            };

            static const std::map<json::json_pointer, std::function<json(int, const std::vector<VSNodeRef *> &, const VSAPI *)>> builtins = {
                { json::json_pointer("/N"), [](int n, const std::vector<VSNodeRef *> &ns, const VSAPI *) -> json { return n; } },
            };

            std::map<json::json_pointer, json> cache;
            cache.emplace(json::json_pointer(""), nullptr);
            auto fget = [&](const json::json_pointer &ptr) -> const json& {
                auto it = cache.find(ptr);
                if (it != cache.end()) return it->second;

                json val = nullptr;
                {
                    auto it = builtins.find(ptr);
                    if (it != builtins.end()) {
                        val = it->second(n, d->nodes, vsapi);
                        return cache[ptr] = val;
                    }
                }

                auto &tokens = ptr.reference_tokens;
                if (tokens.size() < 2) return cache[json::json_pointer("")];
                auto clip = tokens[0], pname = tokens[1];
                int index = extractClipId(clip);
                if (index >= maps.size())
                    throw std::runtime_error(ptr.to_string() + " clip out of range");
                if (maps[index] == nullptr)
                    maps[index] = vsapi->getFramePropsRO(srcs[index]);
                const VSMap *map = maps[index];

                char type = vsapi->propGetType(map, pname.c_str());
                int numElements = vsapi->propNumElements(map, pname.c_str());
                if (type == ptInt) {
                    const int64_t *intArr = vsapi->propGetIntArray(map, pname.c_str(), nullptr);
                    if (tokens.size() == 2 && numElements > 1) {
                        for (int i = 0; i < numElements; i++)
                            val += intArr[i];
                    } else {
                        int idx = tokens.size() < 3 ? 0 : extractIndex(tokens[2]);
                        if (idx < numElements)
                            val = intArr[idx];
                    }
                } else if (type == ptFloat) {
                    const double *floatArr = vsapi->propGetFloatArray(map, pname.c_str(), nullptr);
                    if (tokens.size() == 2 && numElements > 1) {
                        for (int i = 0; i < numElements; i++)
                            val += floatArr[i];
                    } else {
                        int idx = tokens.size() < 3 ? 0 : extractIndex(tokens[2]);
                        if (idx < numElements)
                            val = floatArr[idx];
                    }
                } else if (type == ptData) {
                    if (tokens.size() == 2 && numElements > 1) {
                        for (int idx = 0; idx < numElements; idx++) {
                            const char *value = vsapi->propGetData(map, pname.c_str(), idx, nullptr);
                            int size = vsapi->propGetDataSize(map, pname.c_str(), idx, nullptr);
                            val += std::string(value, size);
                        }
                    } else {
                        int idx = tokens.size() < 3 ? 0 : extractIndex(tokens[2]);
                        if (idx < numElements) {
                            const char *value = vsapi->propGetData(map, pname.c_str(), idx, nullptr);
                            int size = vsapi->propGetDataSize(map, pname.c_str(), idx, nullptr);
                            val = std::string(value, size);
                        }
                    }
                } else if (type == ptFrame) {
                    std::string text = std::to_string(numElements) + " frame";
                    if (numElements != 1)
                        text += 's';
                    val = text;
                } else if (type == ptNode) {
                    std::string text = std::to_string(numElements) + " node";
                    if (numElements != 1)
                        text += 's';
                    val = text;
                } else if (type == ptFunction) {
                    std::string text = std::to_string(numElements) + " function";
                    if (numElements != 1)
                        text += 's';
                    val = text;
                } else if (type == ptUnset) {
                    return cache[json::json_pointer("")];
                }
                return cache[ptr] = val;
            };

            data_provider prov(
                [&](const json::json_pointer &ptr) -> bool {
                    auto &x = fget(ptr);
                    return x != nullptr;
                },
                fget
            );

            for (int i = 0; i < d->tmpl.size(); i++) {
                try {
                    out[i] = d->env.render(d->tmpl[i], prov);
                } catch (inja::InjaError &e) {
                    inja::InjaError e2(e.type, std::string("[prop ") + d->propName[i] + "] " + e.message, e.location);
                    /*if (d->strict)*/ throw e2;
                    //out += "{{template error: " + e2.what() + "}}";
                }
            }
        } catch (std::runtime_error &e) {
            for (auto f: srcs)
                vsapi->freeFrame(f);
            vsapi->setFilterError((std::string("Tmpl(): ") + e.what()).c_str(), frameCtx);
            return nullptr;
        }

        VSFrameRef *dst = vsapi->copyFrame(src, core);
        VSMap *map = vsapi->getFramePropsRW(dst);
        for (int i = 0; i < out.size(); i++)
            vsapi->propSetData(map, d->propName[i].c_str(), out[i].data(), out[i].size(), paReplace);

        for (auto f: srcs)
            vsapi->freeFrame(f);

        return dst;
    }

    return nullptr;
}


static void VS_CC tmplFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TmplData *d = static_cast<TmplData *>(instanceData);
    for (auto p: d->nodes)
        vsapi->freeNode(p);
    delete d;
}


static void VS_CC tmplCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<TmplData> d(new TmplData);

    try {
        int numclips = vsapi->propNumElements(in, "clips");
        for (int i = 0; i < numclips; i++) {
            auto node = vsapi->propGetNode(in, "clips", i, nullptr);
            d->nodes.push_back(node);
        }
        d->vi = vsapi->getVideoInfo(d->nodes[0]);
        int n1 = vsapi->propNumElements(in, "text"), n2 = vsapi->propNumElements(in, "prop");
        if (n1 != n2)
            throw std::runtime_error("text and prop must be paired");
        for (int i = 0; i < n1; i++) {
            d->text.push_back(vsapi->propGetData(in, "text", i, nullptr));
            d->propName.push_back(vsapi->propGetData(in, "prop", i, nullptr));
            try {
                d->tmpl.push_back(d->env.parse(d->text[i]));
            } catch (inja::InjaError &e) {
                inja::InjaError e2(e.type, std::string("[prop ") + d->propName[i] + "] " + e.message, e.location);
                throw e2;
            }
        }
    } catch (std::runtime_error &e) {
        for (auto p: d->nodes)
            vsapi->freeNode(p);
        vsapi->setError(out, e.what());
        return;
    }

    vsapi->createFilter(in, out, "Tmpl", tmplInit, tmplGetFrame, tmplFree, fmParallel, 0, d.release(), core);
}

static void VS_CC versionCreate(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi) {
    for (const auto &f : features)
        vsapi->propSetData(out, "tmpl_features", f.c_str(), -1, paAppend);
}

} // namespace

#ifndef STANDALONE_TMPL
void VS_CC tmplInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    registerVersionFunc(versionCreate);
#else
VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("info.akarin.plugin", "akarin2", "Text Template plugin", VAPOURSYNTH_API_VERSION, 1, plugin);
#endif
    registerFunc("Tmpl",
        "clips:clip[];"
        "prop:data[];"
        "text:data[];"
        , tmplCreate, nullptr, plugin);
}
