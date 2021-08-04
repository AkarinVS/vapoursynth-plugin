#include <memory>
#include <mutex>
#include <utility>
#include <string>
#include <stdexcept>
#include <vector>
#include <stdio.h>

#include "VapourSynth.h"
#include "VSHelper.h"

#ifndef _WIN32
#error "Unsupported platform"
#else
static std::vector<std::string> autoDllErrors;
static const wchar_t *dllPath(const wchar_t *suffix);
#define NGX_DLL dllPath(L".dlisr.dll"),".dlisr.dll",autoDllErrors
#define CUDA_DLL L"nvcuda.dll","nvcuda.dll",autoDllErrors
#endif
#include "cuda.h"
#include "ngx.h"

#define CK_NGX(x) do { \
    int r = (x); \
    if (r != NVSDK_NGX_Result_Success) { \
        fprintf(stderr, "failed NGX call %s: %x at line %d\n", #x, r, __LINE__); \
        abort(); \
    } \
} while (0)
#define CK_CUDA(x) do { \
    int r = (x); \
    if (r != CUDA_SUCCESS) { \
        fprintf(stderr, "failed cuda call %s: %d at line %d\n", #x, r, __LINE__); \
        abort(); \
    } \
} while (0)

static const wchar_t *dllPath(const wchar_t *suffix) {
    static const std::wstring res = [suffix]() -> std::wstring {
        HMODULE mod = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (char *)dllPath, &mod)) {
            std::vector<wchar_t> buf;
            size_t n = 0;
            do {
                buf.resize(buf.size() + MAX_PATH);
                n = GetModuleFileNameW(mod, buf.data(), buf.size());
            } while (n >= buf.size());
            buf.resize(n);
            std::wstring path(buf.begin(), buf.end() - 4);
            path += suffix;
            return path;
        }
        throw std::runtime_error("unable to locate myself");
    }();
    return res.c_str();
}

static void *cudaMalloc(size_t size) {
    void *ptr = nullptr;
    CK_CUDA(cuMemAlloc_v2(&ptr, size));
    return ptr;
}

struct NgxData {
    std::mutex lock;

    VSNodeRef *node;
    VSVideoInfo vi;
    int scale;

    typedef float T;
    uint64_t pixel_size() const { return 3 * sizeof(T); }
    uint64_t in_image_width() const   { return vi.width / scale; }
    uint64_t out_image_width() const  { return vi.width; }
    uint64_t in_image_height() const  { return vi.height / scale; }
    uint64_t out_image_height() const { return vi.height; }
    uint64_t in_image_row_bytes() const  { return pixel_size() * in_image_width(); }
    uint64_t out_image_row_bytes() const { return pixel_size() * out_image_width(); }
    uint64_t in_size() const  { return in_image_height()  * in_image_row_bytes(); }
    uint64_t out_size() const { return out_image_height() * out_image_row_bytes(); }

    NVSDK_NGX_Parameter *param;
    NVSDK_NGX_Handle *DUHandle;
    CUcontext ctx;

    std::vector<uint8_t> in_host, out_host;
    CUdeviceptr inp, outp;
    void allocate() {
        in_host.resize(in_size());
        out_host.resize(out_size());
        inp = cudaMalloc(in_size());
        outp = cudaMalloc(out_size());
    }

    NgxData() : node(nullptr), vi(), scale(0), param(nullptr), DUHandle(nullptr), ctx(nullptr), inp(nullptr), outp(nullptr) {}
    ~NgxData() {
        if (ctx) {
            CK_CUDA(cuCtxPushCurrent(ctx));
            if (inp) CK_CUDA(cuMemFree_v2(inp));
            if (outp) CK_CUDA(cuMemFree_v2(outp));
            if (DUHandle) CK_NGX(NVSDK_NGX_CUDA_ReleaseFeature(DUHandle));
            cuCtxPopCurrent(nullptr);
        }
    }
};

static void VS_CC ngxInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    NgxData *d = static_cast<NgxData *>(*instanceData);
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC ngxGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    NgxData *d = static_cast<NgxData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);

        const VSFormat *fi = d->vi.format;
        assert(vsapi->getFrameHeight(src, 0) == (int)d->in_image_height());
        assert(vsapi->getFrameWidth(src, 0) == (int)d->in_image_width());
        int planes[3] = { 0, 1, 2 };
        const VSFrameRef *srcf[3] = { nullptr, nullptr, nullptr };
        VSFrameRef *dst = vsapi->newVideoFrame2(fi, d->out_image_width(), d->out_image_height(), srcf, planes, src, core);

        // The NGX API is not thread safe.
        std::lock_guard<std::mutex> lock(d->lock);
        CK_CUDA(cuCtxPushCurrent(d->ctx));

        auto params = d->param;
        params->Set(NVSDK_NGX_Parameter_Width, (uint64_t)d->in_image_width());
        params->Set(NVSDK_NGX_Parameter_Height, (uint64_t)d->in_image_height());
        params->Set(NVSDK_NGX_Parameter_Scale, d->scale);

        // Create the feature
        //CK_NGX(NVSDK_NGX_CUDA_CreateFeature(NVSDK_NGX_Feature_ImageSuperResolution, params, &d->DUHandle));

        void *in_image_dev_ptr = d->inp;
        void *out_image_dev_ptr = d->outp;;

        uint8_t *host = d->in_host.data();
        typedef float T;
        const T factor = 255.0f;
        for (int plane = 0; plane < 3; plane++) {
            const size_t stride = vsapi->getStride(src, plane);
            const uint8_t *ptr = (uint8_t*)vsapi->getReadPtr(src, plane);
            for (size_t i = 0; i < d->in_image_height(); i++)
                for (size_t j = 0; j < d->in_image_width(); j++)
                    *(T*)&host[i * d->in_image_row_bytes() + j * d->pixel_size() + plane * sizeof(T)] = *(T*)&ptr[i * stride + j * sizeof(T)] * factor;
        }
        CK_CUDA(cuMemcpyHtoD_v2(in_image_dev_ptr, host, d->in_size()));

        // Pass the pointers to the GPU allocations to the
        // parameter block along with the format and size.
        params->Set(NVSDK_NGX_Parameter_Color_SizeInBytes, d->in_size());
        params->Set(NVSDK_NGX_Parameter_Color_Format, NVSDK_NGX_Buffer_Format_RGB32F);
        params->Set(NVSDK_NGX_Parameter_Color, in_image_dev_ptr);
        params->Set(NVSDK_NGX_Parameter_Output_SizeInBytes, d->out_size());
        params->Set(NVSDK_NGX_Parameter_Output_Format, NVSDK_NGX_Buffer_Format_RGB32F);
        params->Set(NVSDK_NGX_Parameter_Output, out_image_dev_ptr);

        // Execute the feature.
        CK_NGX(NVSDK_NGX_CUDA_EvaluateFeature(d->DUHandle, params, nullptr));

        host = d->out_host.data();
        CK_CUDA(cuMemcpyDtoH_v2(host, out_image_dev_ptr, d->out_size()));
        for (int plane = 0; plane < 3; plane++) {
            const size_t stride = vsapi->getStride(dst, plane);
            uint8_t *ptr = (uint8_t*)vsapi->getWritePtr(dst, plane);
            for (size_t i = 0; i < d->out_image_height(); i++)
                for (size_t j = 0; j < d->out_image_width(); j++)
                    *(T*)&ptr[i * stride + j * sizeof(T)] = *(T*)&host[i * d->out_image_row_bytes() + j * d->pixel_size() + plane * sizeof(T)] / factor;
        }

        cuCtxPopCurrent(nullptr);

        vsapi->freeFrame(src);
        return dst;
    }

    return nullptr;
}

static void VS_CC ngxFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    NgxData *d = static_cast<NgxData *>(instanceData);
    vsapi->freeNode(d->node);

    delete d;
}

static void VS_CC ngxCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<NgxData> d(new NgxData);
    int err;

    int devid = 0;
    try {
        if (autoDllErrors.size() > 0) {
                std::string error, last;
                for (const auto &s: autoDllErrors) {
                    if (error.size()) {
                        if (last != s)
                            error += "; " + s;
                    } else
                        error = s;
                    last = s;
                }
                throw std::runtime_error(error);
        }

        d->node = vsapi->propGetNode(in, "clip", 0, &err);
        d->vi = *vsapi->getVideoInfo(d->node);

        if (!isConstantFormat(&d->vi)) {
            throw std::runtime_error("Only clips with constant format and dimensions allowed");
        }
        if (d->vi.format->numPlanes != 3 || d->vi.format->colorFamily != cmRGB)
            throw std::runtime_error("input clip must be RGB format");
        if (d->vi.format->sampleType != stFloat || d->vi.format->bitsPerSample != 32)
            throw std::runtime_error("input clip must be 32-bit float format");

        int scale = int64ToIntS(vsapi->propGetInt(in, "scale", 0, &err));
        if (err) scale = 2;
        if (scale != 2 && scale != 4 && scale != 8)
            throw std::runtime_error("scale must be 2/4/8");
        d->scale = scale;

        devid = int64ToIntS(vsapi->propGetInt(in, "device_id", 0, &err));
        if (err) devid = 0;
    } catch (std::runtime_error &e) {
        if (d->node)
            vsapi->freeNode(d->node);
        vsapi->setError(out, (std::string{ "DLISR: " } + e.what()).c_str());
        return;
    }

    d->vi.width *= d->scale;
    d->vi.height *= d->scale;

    static bool inited = []() -> bool {
        CUcontext ctx;
        bool hasCtx = cuCtxGetCurrent(&ctx) == CUDA_SUCCESS;
        CK_NGX(NVSDK_NGX_CUDA_Init(0, L"./", NVSDK_NGX_Version_API));
        // We don't expect NVSDK_NGX_CUDA_Init to create a context, but if it did, we need to
        // switch to save a global CUDA context, instead of the pre-filter context.
        if (!hasCtx && cuCtxGetCurrent(&ctx) == CUDA_SUCCESS) {
            fprintf(stderr, "invariant violated: NVSDK_NGX_CUDA_Init created CUDA context: %p\n", ctx);
            abort();
        }
        return true;
    }();
    (void) inited;
    NV_new_Parameter(&d->param);

    d->param->Set(NVSDK_NGX_Parameter_Width, d->in_image_width());
    d->param->Set(NVSDK_NGX_Parameter_Height, d->in_image_height());
    d->param->Set(NVSDK_NGX_Parameter_Scale, d->scale);

    // Get the scratch buffer size and create the scratch allocation.
    size_t byteSize{ 0u };
    CK_NGX(NVSDK_NGX_CUDA_GetScratchBufferSize(NVSDK_NGX_Feature_ImageSuperResolution, d->param, &byteSize));
    if (byteSize != 0) // should request none.
        abort();

    // Create the feature
    CUdevice dev = 0;
    CK_CUDA(cuInit(0));
    CK_CUDA(cuDeviceGet(&dev, devid));
    CK_CUDA(cuCtxCreate_v2(&d->ctx, 0, dev));
    CK_NGX(NVSDK_NGX_CUDA_CreateFeature(NVSDK_NGX_Feature_ImageSuperResolution, d->param, &d->DUHandle));
    CK_CUDA(cuCtxGetCurrent(&d->ctx));
    d->allocate();
    CK_CUDA(cuCtxPopCurrent(nullptr));

    vsapi->createFilter(in, out, "DLISR", ngxInit, ngxGetFrame, ngxFree, fmParallel, 0, d.release(), core);
}

//////////////////////////////////////////
// Init

#ifndef STANDALONE_NGX
void VS_CC ngxInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
#else
VS_EXTERNAL_API(void) VS_CC VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("info.akarin.plugin", "akarin2", "Experimental Nvidia DLISR plugin", VAPOURSYNTH_API_VERSION, 1, plugin);
#endif
    registerFunc("DLISR", "clip:clip;scale:int:opt;device_id:int:opt;", ngxCreate, nullptr, plugin);
}
