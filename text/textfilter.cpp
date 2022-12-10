/*
* Copyright (c) 2013-2014 John Smith
* Copyright (c) 2022-     Akarin
*
*
* VapourSynth is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* VapourSynth is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with VapourSynth; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include <cstdint>
#include <cstdlib>

#include <functional>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include <VapourSynth.h>
#include <VSHelper.h>

#define FMT_HEADER_ONLY
#include "fmt/args.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

#include "filtershared.h"
#include "ter-116n.h"
#include "../plugin.h"

static const std::string clipNamePrefix { "src" };

std::vector<std::string> features = {
    "x.property", "{}",
    clipNamePrefix + "0", clipNamePrefix + "26",
};

const int margin_h = 16;
const int margin_v = 16;

namespace {
typedef std::vector<std::string> stringlist;
} // namespace

static void scrawl_character_int(unsigned char c, uint8_t *image, int stride, int dest_x, int dest_y, int bitsPerSample, int scale) {
    int black = 16 << (bitsPerSample - 8);
    int white = 235 << (bitsPerSample - 8);
    int x, y;
    if (bitsPerSample == 8) {
        for (y = 0; y < character_height * scale; y++) {
            for (x = 0; x < character_width * scale; x++) {
                if (__font_bitmap__[c * character_height + y/scale] & (1 << (7 - x/scale))) {
                    image[dest_y*stride + dest_x + x] = white;
                } else {
                    image[dest_y*stride + dest_x + x] = black;
                }
            }

            dest_y++;
        }
    } else {
        for (y = 0; y < character_height * scale; y++) {
            for (x = 0; x < character_width * scale; x++) {
                if (__font_bitmap__[c * character_height + y/scale] & (1 << (7 - x/scale))) {
                    reinterpret_cast<uint16_t *>(image)[dest_y*stride/2 + dest_x + x] = white;
                } else {
                    reinterpret_cast<uint16_t *>(image)[dest_y*stride/2 + dest_x + x] = black;
                }
            }

            dest_y++;
        }
    }
}


static void scrawl_character_float(unsigned char c, uint8_t *image, int stride, int dest_x, int dest_y, int scale) {
    float white = 1.0f;
    float black = 0.0f;
    int x, y;

    for (y = 0; y < character_height * scale; y++) {
        for (x = 0; x < character_width * scale; x++) {
            if (__font_bitmap__[c * character_height + y/scale] & (1 << (7 - x/scale))) {
                reinterpret_cast<float *>(image)[dest_y*stride/4 + dest_x + x] = white;
            } else {
                reinterpret_cast<float *>(image)[dest_y*stride/4 + dest_x + x] = black;
            }
        }

        dest_y++;
    }
}

static void sanitise_text(std::string& txt) {
    for (size_t i = 0; i < txt.length(); i++) {
        if (txt[i] == '\r') {
            if (txt[i+1] == '\n') {
                txt.erase(i, 1);
            } else {
                txt[i] = '\n';
            }
            continue;
        } else if (txt[i] == '\n') {
            continue;
        }

        // Must adjust the character code because of the five characters
        // missing from the font.
        unsigned char current_char = static_cast<unsigned char>(txt[i]);
        if (current_char < 32 ||
            current_char == 129 ||
            current_char == 141 ||
            current_char == 143 ||
            current_char == 144 ||
            current_char == 157) {
                txt[i] = '_';
                continue;
        }

        if (current_char > 157) {
            txt[i] -= 5;
        } else if (current_char > 144) {
            txt[i] -= 4;
        } else if (current_char > 141) {
            txt[i] -= 2;
        } else if (current_char > 129) {
            txt[i] -= 1;
        }
    }
}


static stringlist split_text(const std::string& txt, int width, int height, int scale) {
    stringlist lines;

    // First split by \n
    size_t prev_pos = -1;
    for (size_t i = 0; i < txt.length(); i++) {
        if (txt[i] == '\n') {
            //if (i > 0 && i - prev_pos > 1) { // No empty lines allowed
            lines.push_back(txt.substr(prev_pos + 1, i - prev_pos - 1));
            //}
            prev_pos = i;
        }
    }
    lines.push_back(txt.substr(prev_pos + 1));

    // Then split any lines that don't fit
    size_t horizontal_capacity = width / character_width / scale;
    for (stringlist::iterator iter = lines.begin(); iter != lines.end(); iter++) {
        if (iter->size() > horizontal_capacity) {
            iter = std::prev(lines.insert(std::next(iter), iter->substr(horizontal_capacity)));
            iter->erase(horizontal_capacity);
        }
    }

    // Also drop lines that would go over the frame's bottom edge
    size_t vertical_capacity = height / character_height / scale;
    if (lines.size() > vertical_capacity) {
        lines.resize(vertical_capacity);
    }

    return lines;
}


static void scrawl_text(std::string txt, int alignment, int scale, VSFrameRef *frame, const VSAPI *vsapi) {
    const VSFormat *frame_format = vsapi->getFrameFormat(frame);
    int width = vsapi->getFrameWidth(frame, 0);
    int height = vsapi->getFrameHeight(frame, 0);

    sanitise_text(txt);

    stringlist lines = split_text(txt, width - margin_h*2, height - margin_v*2, scale);

    int start_x = 0;
    int start_y = 0;

    switch (alignment) {
    case 7:
    case 8:
    case 9:
        start_y = margin_v;
        break;
    case 4:
    case 5:
    case 6:
        start_y = (height - static_cast<int>(lines.size())*character_height*scale) / 2;
        break;
    case 1:
    case 2:
    case 3:
        start_y = height - static_cast<int>(lines.size())*character_height*scale - margin_v;
        break;
    }

    for (const auto &iter : lines) {
        switch (alignment) {
        case 1:
        case 4:
        case 7:
            start_x = margin_h;
            break;
        case 2:
        case 5:
        case 8:
            start_x = (width - static_cast<int>(iter.size())*character_width*scale) / 2;
            break;
        case 3:
        case 6:
        case 9:
            start_x = width - static_cast<int>(iter.size())*character_width*scale - margin_h;
            break;
        }

        for (size_t i = 0; i < iter.size(); i++) {
            int dest_x = start_x + static_cast<int>(i)*character_width*scale;
            int dest_y = start_y;

            if (frame_format->colorFamily == cmRGB) {
                for (int plane = 0; plane < frame_format->numPlanes; plane++) {
                    uint8_t *image = vsapi->getWritePtr(frame, plane);
                    int stride = vsapi->getStride(frame, plane);

                    if (frame_format->sampleType == stInteger) {
                        scrawl_character_int(iter[i], image, stride, dest_x, dest_y, frame_format->bitsPerSample, scale);
                    } else {
                        scrawl_character_float(iter[i], image, stride, dest_x, dest_y, scale);
                    }
                }
            } else {
                for (int plane = 0; plane < frame_format->numPlanes; plane++) {
                    uint8_t *image = vsapi->getWritePtr(frame, plane);
                    int stride = vsapi->getStride(frame, plane);

                    if (plane == 0) {
                        if (frame_format->sampleType == stInteger) {
                            scrawl_character_int(iter[i], image, stride, dest_x, dest_y, frame_format->bitsPerSample, scale);
                        } else {
                            scrawl_character_float(iter[i], image, stride, dest_x, dest_y, scale);
                        }
                    } else {
                        int sub_w = scale * character_width  >> frame_format->subSamplingW;
                        int sub_h = scale * character_height >> frame_format->subSamplingH;
                        int sub_dest_x = dest_x >> frame_format->subSamplingW;
                        int sub_dest_y = dest_y >> frame_format->subSamplingH;
                        int y;

                        if (frame_format->bitsPerSample == 8) {
                            for (y = 0; y < sub_h; y++) {
                                memset(image + (y+sub_dest_y)*stride + sub_dest_x, 128, sub_w);
                            }
                        } else if (frame_format->bitsPerSample <= 16) {
                            for (y = 0; y < sub_h; y++) {
                                vs_memset16(reinterpret_cast<uint16_t *>(image) + (y+sub_dest_y)*stride/2 + sub_dest_x, 128 << (frame_format->bitsPerSample - 8), sub_w);
                            }
                        } else {
                            for (y = 0; y < sub_h; y++) {
                                vs_memset_float(reinterpret_cast<float *>(image) + (y+sub_dest_y)*stride/4 + sub_dest_x, 0.0f, sub_w);
                            }
                        }
                    } // if plane
                } // for plane in planes
            } // if colorFamily
        } // for i in line
        start_y += character_height * scale;
    } // for iter in lines
}


namespace {

struct PropAccess {
    std::string id;
    std::string name;
    int index;
    PropAccess(const std::string &id, int index, const std::string &name): id(id), name(name), index(index) {}
};

typedef struct {
    std::vector<VSNodeRef *> nodes;
    const VSVideoInfo *vi;

    std::string text;
    std::vector<PropAccess> pa;
    std::string propName;
    int alignment;
    int scale;
    bool vspipe;
    bool strict;
} TextData;

struct CustomValue {
    int val;
    std::string (*fptr)(int);
    CustomValue(int val, std::string (*fptr)(int)): val(val), fptr(fptr) {}
};

struct MissingValue {
    std::string s;
    MissingValue(std::string s): s(s) {}
};

} // namespace

FMT_BEGIN_NAMESPACE
template <> struct formatter<CustomValue>: public formatter<int>, formatter<string_view> {
    bool useFunc;
    auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
        auto it = ctx.begin(), end = std::find(it, ctx.end(), '}');
        useFunc = true;
        if (it == end) return it;
        if (*(end-1) == 's') {
            return formatter<string_view>::parse(ctx);
        }
        useFunc = false;
        return formatter<int>::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const CustomValue &p, FormatContext &ctx) -> decltype(ctx.out()) {
        if (useFunc) {
            std::string s = p.fptr(p.val);
            return formatter<string_view>::format(s, ctx);
        }
        return formatter<int>::format(p.val, ctx);
    }
};
template <> struct formatter<MissingValue>: public formatter<int>, formatter<string_view> {
    bool asString;
    auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
        auto it = ctx.begin(), end = std::find(it, ctx.end(), '}');
        asString = true;
        if (it == end) return it;
        if (*(end-1) == 's') {
            return formatter<string_view>::parse(ctx);
        }
        asString = false;
        return formatter<int>::parse(ctx);
    }

    template <typename FormatContext>
    auto format(const MissingValue &p, FormatContext &ctx) -> decltype(ctx.out()) {
        if (asString)
            return formatter<string_view>::format(p.s, ctx);
        return formatter<int>::format(0, ctx);
    }
};
FMT_END_NAMESPACE

static void VS_CC textInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    TextData *d = static_cast<TextData *>(*instanceData);
    vsapi->setVideoInfo(d->vi, 1, node);
}


static std::string fieldBasedToString(int field) {
    std::string s;
    if (field == 0)
        s = "Frame based";
    else if (field == 1)
        s = "Bottom field first";
    else if (field == 2)
        s = "Top field first";
    else
        s = fmt::format("FieldBased({})", field);
    return s;
}

static std::string colorFamilyToString(int cf) {
    std::string family;
    if (cf == cmGray)
        family = "Gray";
    else if (cf == cmRGB)
        family = "RGB";
    else if (cf == cmYUV)
        family = "YUV";
    else if (cf == cmYCoCg)
        family = "YCoCg";
    else if (cf == cmCompat)
        family = "Compat";
    else
        family = fmt::format("ColorFamily({})", cf);
    return family;
}

static std::string chromaLocationToString(int location) {
    std::string s;
    if (location == 0)
        s = "Left";
    else if (location == 1)
        s = "Center";
    else if (location == 2)
        s = "Top left";
    else if (location == 3)
        s = "Top";
    else if (location == 4)
        s = "Bottom left";
    else if (location == 5)
        s = "Bottom";
    else
        s = fmt::format("Location({})", location);
    return s;
}

static std::string rangeToString(int range) {
    std::string s;
    if (range == 0)
        s = "Full range";
    else if (range == 1)
        s = "Limited range";
    else
        s = fmt::format("Range({})", range);
    return s;
}

static std::string matrixToString(int matrix) {
    std::string s;
    if (matrix == 0)
        s = "sRGB";
    else if (matrix == 1)
        s = "BT.709";
    else if (matrix == 4)
        s = "FCC";
    else if (matrix == 5 || matrix  == 6)
        s = "BT.601";
    else if (matrix == 7)
        s = "SMPTE 240M";
    else if (matrix == 8)
        s = "YCoCg";
    else if (matrix == 9)
        s = "BT.2020 NCL";
    else if (matrix == 10)
        s = "BT.2020 CL";
    else if (matrix == 11)
        s = "SMPTE 2085";
    else if (matrix == 12)
        s = "Cromaticity dervived cl";
    else if (matrix == 13)
        s = "Cromaticity dervived ncl";
    else if (matrix == 14)
        s = "ICtCp";
    else
        s = fmt::format("Matrix({})", matrix);
    return s;
}

static std::string primariesToString(int primaries) {
    std::string s;
    if (primaries == 1)
        s = "BT.709";
    else if (primaries == 4)
        s = "BT.470M";
    else if (primaries == 5)
        s = "BT.470BG";
    else if (primaries == 6)
        s = "SMPTE 170M";
    else if (primaries == 7)
        s = "SMPTE 240M";
    else if (primaries == 8)
        s = "FILM";
    else if (primaries == 9)
        s = "BT.2020";
    else if (primaries == 10)
        s = "SMPTE 428";
    else if (primaries == 11)
        s = "SMPTE 431";
    else if (primaries == 12)
        s = "SMPTE 432";
    else if (primaries == 22)
        s = "JEDEC P22";
    else
        s = fmt::format("Primaries({})", primaries);
    return s;
}

static std::string transferToString(int transfer) {
        std::string s;
        if (transfer == 1)
            s = "BT.709";
        else if (transfer == 4)
            s = "Gamma 2.2";
        else if (transfer == 5)
            s = "Gamma 2.8";
        else if (transfer == 6)
            s = "SMPTE 170M";
        else if (transfer == 7)
            s = "SMPTE 240M";
        else if (transfer == 8)
            s = "Linear";
        else if (transfer == 9)
            s = "Logaritmic (100:1 range)";
        else if (transfer == 10)
            s = "Logaritmic (100 * Sqrt(10) : 1 range)";
        else if (transfer == 11)
            s = "IEC 61966-2-4";
        else if (transfer == 12)
            s = "BT.1361 Extended Colour Gamut";
        else if (transfer == 13)
            s = "IEC 61966-2-1";
        else if (transfer == 14)
            s = "BT.2020 for 10 bit system";
        else if (transfer == 15)
            s = "BT.2020 for 12 bit system";
        else if (transfer == 16)
            s = "SMPTE 2084";
        else if (transfer == 17)
            s = "SMPTE 428";
        else if (transfer == 18)
            s = "ARIB STD-B67";
        else
            s = fmt::format("Transfer({})", transfer);
        return s;
}

using dynamic_format_arg_store = fmt::dynamic_format_arg_store<fmt::format_context>;

std::vector<PropAccess> checkFormatString(const std::string f) {
    struct bitbucket {
        typedef char value_type;
        void push_back(char) {}
    };
    std::vector<PropAccess> pa;
    dynamic_format_arg_store store;
    store.push_back(fmt::arg("N", -1)); // builtin
    static const std::regex framePropRe { "^([a-z]|" + clipNamePrefix + "[0-9]+)\\.([^\\[\\]]*)$" };
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
    std::smatch match;
    while (1) {
        bitbucket null;
        try {
            vformat_to(std::back_inserter(null), f, store);
        } catch (fmt::missing_arg &e) {
            std::string id = e.what();
            store.push_back(fmt::arg(id.c_str(), CustomValue(1.0, matrixToString)));
            if (std::regex_match(id, match, framePropRe)) {
                auto clip = match[1].str(), name = match[2].str();
                int clipi = extractClipId(clip);
                pa.emplace_back(id, clipi, name);
            } else {
                pa.emplace_back(id, 0, id);
            }
            continue;
        } catch (fmt::format_error &e) {
            throw e;
        }
        break;
    }
    return pa;
}

template<typename T>
class vector_view {
    const T *ptr;
    size_t n;
public:
    vector_view(const T *ptr, size_t n): ptr(ptr), n(n) {}
    const T *begin() const { return ptr; }
    const T *end() const { return ptr + n; }
};

static void pushArg(const PropAccess &pa, dynamic_format_arg_store &store, const std::vector<const VSMap *> &maps, const VSAPI *vsapi) {
    const VSMap *map = maps[pa.index];
    int err;
#define CHECK_ARG(pname, pfunc) do { \
    if (pa.name == pname) { \
        int val = int64ToIntS(vsapi->propGetInt(map, pname, 0, &err)); \
        if (err) val = -1; \
        store.push_back(fmt::arg(pa.id.c_str(), CustomValue(val, pfunc))); \
        return; \
    } \
    } while (0)

    CHECK_ARG("_Matrix", matrixToString);
    CHECK_ARG("_Primaries", primariesToString);
    CHECK_ARG("_Transfer", transferToString);
    CHECK_ARG("_ColorRange", rangeToString);
    CHECK_ARG("_ChromaLocation", chromaLocationToString);
    CHECK_ARG("_FieldBased", fieldBasedToString);
#undef CHECK_ARG

    if (pa.name == "_PictType") {
        const char *picttype = vsapi->propGetData(map, "_PictType", 0, &err);
        store.push_back(fmt::arg(pa.id.c_str(), picttype ? picttype : "Unknown"));
        return;
    }

    const char *key = pa.name.c_str();
    auto type = vsapi->propGetType(map, key);
    switch (type) {
    case ptInt: {
        int n = vsapi->propNumElements(map, key);
        if (n == 1)
            store.push_back(fmt::arg(pa.id.c_str(), vsapi->propGetInt(map, key, 0, nullptr)));
        else
            store.push_back(fmt::arg(pa.id.c_str(), vector_view<int64_t>(vsapi->propGetIntArray(map, key, nullptr), n)));
        break;
    }
    case ptFloat: {
        int n = vsapi->propNumElements(map, key);
        if (n == 1)
            store.push_back(fmt::arg(pa.id.c_str(), vsapi->propGetFloat(map, key, 0, nullptr)));
        else
            store.push_back(fmt::arg(pa.id.c_str(), vector_view<double>(vsapi->propGetFloatArray(map, key, nullptr), n)));
        break;
    }
    case ptData:
        store.push_back(fmt::arg(pa.id.c_str(), vsapi->propGetData(map, key, 0, nullptr)));
        break;
    case ptUnset:
        store.push_back(fmt::arg(pa.id.c_str(), MissingValue("<missing key>")));
        break;
    case ptNode:
        store.push_back(fmt::arg(pa.id.c_str(), MissingValue("<node")));
        break;
    case ptFrame:
        store.push_back(fmt::arg(pa.id.c_str(), MissingValue("<frame>")));
        break;
    case ptFunction:
        store.push_back(fmt::arg(pa.id.c_str(), MissingValue("<func>")));
        break;
    default:
        throw std::runtime_error(fmt::format("propGetType({}) returned {}, should not happen", key, type));
        break;
    }
}

static const VSFrameRef *VS_CC textGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    TextData *d = static_cast<TextData *>(*instanceData);

    if (activationReason == arInitial) {
        for (auto node: d->nodes)
            vsapi->requestFrameFilter(n, node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        std::vector<const VSFrameRef *> srcs;
        const VSFrameRef *src = nullptr;
        auto out = fmt::memory_buffer();
        try {
            for (auto node: d->nodes) {
                auto f = vsapi->getFrameFilter(n, node, frameCtx);
                srcs.push_back(f);
                const VSFormat *frame_format = vsapi->getFrameFormat(f);
                if ((frame_format->sampleType == stInteger && frame_format->bitsPerSample > 16) ||
                        (frame_format->sampleType == stFloat && frame_format->bitsPerSample != 32)) {
                    throw std::runtime_error("Only 8..16 bit integer and 32 bit float formats supported");
                }
            }

            src = srcs[0];
            std::vector<const VSMap *> maps(srcs.size(), nullptr);

            dynamic_format_arg_store store;
            store.push_back(fmt::arg("N", n)); // builtin

            for (const auto &pa: d->pa) {
                int index = pa.index;
                if (maps[index] == nullptr)
                    maps[index] = vsapi->getFramePropsRO(srcs[index]);
                pushArg(pa, store, maps, vsapi);
            }

            try {
                vformat_to(std::back_inserter(out), d->text, store);
            } catch (fmt::format_error &e) {
                if (d->strict) throw;
                fmt::format_to(std::back_inserter(out), "{{format error: {}}}", e.what());
            }

            int width = vsapi->getFrameWidth(src, 0);
            int height = vsapi->getFrameHeight(src, 0);

            int minimum_width = 2 * margin_h + character_width * d->scale;
            int minimum_height = 2 * margin_v + character_height * d->scale;

            if (width < minimum_width || height < minimum_height) {
                throw std::runtime_error(fmt::format("frame size ({}x{}) must be at least {}x{} pixels", width, height, minimum_width, minimum_height).c_str());
            }
        } catch (std::runtime_error &e) {
            for (auto f: srcs)
                vsapi->freeFrame(f);
            vsapi->setFilterError((std::string("Text(") + d->text + "): " + e.what()).c_str(), frameCtx);
            return nullptr;
        }

        VSFrameRef *dst = vsapi->copyFrame(src, core);
        if (d->propName.size() == 0) {
            scrawl_text(std::string(out.data(), out.size()), d->alignment, d->scale, dst, vsapi);
        } else {
            VSMap *map = vsapi->getFramePropsRW(dst);
            vsapi->propSetData(map, d->propName.c_str(), out.data(), out.size(), paReplace);
        }

        for (auto f: srcs)
            vsapi->freeFrame(f);

        return dst;
    }

    return nullptr;
}


static void VS_CC textFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TextData *d = static_cast<TextData *>(instanceData);
    for (auto p: d->nodes)
        vsapi->freeNode(p);
    delete d;
}


static void VS_CC textCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<TextData> d(new TextData);
    int err;

    try {
        int numclips = vsapi->propNumElements(in, "clips");
        for (int i = 0; i < numclips; i++) {
            auto node = vsapi->propGetNode(in, "clips", i, &err);
            d->nodes.push_back(node);
        }
        d->vi = vsapi->getVideoInfo(d->nodes[0]);

        if (isCompatFormat(d->vi))
            throw std::runtime_error("Text: Compat formats not supported");

        if (d->vi->format && ((d->vi->format->sampleType == stInteger && d->vi->format->bitsPerSample > 16) ||
            (d->vi->format->sampleType == stFloat && d->vi->format->bitsPerSample != 32))) {
                throw std::runtime_error("Text: Only 8-16 bit integer and 32 bit float formats supported");
        }

        d->alignment = int64ToIntS(vsapi->propGetInt(in, "alignment", 0, &err));
        if (err) {
            d->alignment = 7; // top left
        }

        if (d->alignment < 1 || d->alignment > 9)
            throw std::runtime_error("Text: alignment must be between 1 and 9 (think numpad)");

        d->scale = int64ToIntS(vsapi->propGetInt(in, "scale", 0, &err));
        if (err) {
            d->scale = 1;
        }

        d->text = vsapi->propGetData(in, "text", 0, nullptr);
        d->pa = checkFormatString(d->text);

        for (const auto &pa: d->pa) {
            if (pa.index < 0 || pa.index >= d->nodes.size())
                throw std::runtime_error(fmt::format("Text: {} references to out of bound clip (only {} clips)", pa.id, d->nodes.size()));
        }

        auto propName = vsapi->propGetData(in, "prop", 0, &err);
        if (propName)
            d->propName = propName;
        d->vspipe = vsapi->propGetInt(in, "vspipe", 0, &err);
        d->strict = vsapi->propGetInt(in, "strict", 0, &err);
    } catch (std::runtime_error &e) {
        for (auto p: d->nodes)
            vsapi->freeNode(p);
        vsapi->setError(out, e.what());
        return;
    }

    vsapi->createFilter(in, out, "Text", textInit, textGetFrame, textFree, fmParallel, 0, d.release(), core);
}

static void VS_CC versionCreate(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    for (const auto &f : features)
        vsapi->propSetData(out, "text_features", f.c_str(), -1, paAppend);
}

#ifndef STANDALONE_TEXT
void VS_CC textInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    registerVersionFunc(versionCreate);
#else
VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("info.akarin.plugin", "akarin2", "Format Text plugin", VAPOURSYNTH_API_VERSION, 1, plugin);
#endif
    registerFunc("Text",
        "clips:clip[];"
        "text:data;"
        "alignment:int:opt;"
        "scale:int:opt;"
        "prop:data:opt;"
        "strict:int:opt;"
        "vspipe:int:opt;"
        , textCreate, nullptr, plugin);
}
