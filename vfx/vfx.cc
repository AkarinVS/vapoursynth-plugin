#include <memory>
#include <mutex>
#include <utility>
#include <string>
#include <stdexcept>
#include <vector>
#include <stdio.h>
#include <assert.h>

#include "VapourSynth.h"
#include "VSHelper.h"

#ifndef _WIN32
#error "Unsupported platform"
#else
static std::vector<std::string> autoDllErrors;
#define CUDA_DLL L"nvcuda.dll","nvcuda.dll",autoDllErrors
#endif
#include "../ngx/cuda.h"

#include "nvvfx/include/nvCVStatus.h"
#include "nvvfx/include/nvCVImage.h"
#include "nvvfx/include/nvVideoEffects.h"

#define CK_VFX(x) do { \
    NvCV_Status r = (x); \
    if (r != NVCV_SUCCESS) { \
        fprintf(stderr, "failed VFX call %s: %x (%s)\n", #x, r, NvCV_GetErrorStringFromCode(r)); \
        abort(); \
    } \
} while (0)
#define CK_CUDA(x) do { \
    int r = (x); \
    if (r != CUDA_SUCCESS) { \
        fprintf(stderr, "failed cuda call %s: %d\n", #x, r); \
        abort(); \
    } \
} while (0)

struct VfxData {
    std::mutex lock;

    VSNodeRef *node;
    VSVideoInfo vi;
    double scale;
    double strength;

    int in_width, in_height;

    NvVFX_Handle vfx;
    CUstream stream;
    CUdeviceptr state;

    NvCVImage srcCpuImg, srcGpuImg;
    NvCVImage dstCpuImg, dstGpuImg;

    typedef float T;
    uint64_t in_image_width() const   { return in_width; }
    uint64_t out_image_width() const  { return vi.width; }
    uint64_t in_image_height() const  { return in_height; }
    uint64_t out_image_height() const { return vi.height; }

    VfxData() : node(nullptr), vi(), scale(0), strength(0), vfx(nullptr), stream(nullptr), state(nullptr) {}
    ~VfxData() {
        if (vfx) NvVFX_DestroyEffect(vfx);
        if (stream) NvVFX_CudaStreamDestroy(stream);
        if (state) cuMemFree_v2(state);
        NvCVImage_Dealloc(&srcCpuImg);
        NvCVImage_Dealloc(&srcGpuImg);
        NvCVImage_Dealloc(&dstCpuImg);
        NvCVImage_Dealloc(&dstGpuImg);
    }
};

static void VS_CC vfxInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    VfxData *d = static_cast<VfxData *>(*instanceData);
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC vfxGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    VfxData *d = static_cast<VfxData *>(*instanceData);

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

        std::lock_guard<std::mutex> lock(d->lock);

        typedef VfxData::T T;
        T *host = (T *)d->srcCpuImg.pixels;
        for (int plane = 0; plane < 3; plane++) {
            const size_t stride = vsapi->getStride(src, plane);
            const uint8_t *ptr = (uint8_t*)vsapi->getReadPtr(src, plane);
            const size_t w = d->in_image_width(), h = d->in_image_height();
            for (size_t i = 0; i < h; i++)
                for (size_t j = 0; j < w; j++)
                    host[plane * h * w + i * w + j] = *(T*)&ptr[i * stride + j * sizeof(T)];
        }

        CK_VFX(NvCVImage_Transfer(&d->srcCpuImg, &d->srcGpuImg, 1.0f, d->stream, nullptr));

        CK_VFX(NvVFX_Run(d->vfx, 0));

        CK_VFX(NvCVImage_Transfer(&d->dstGpuImg, &d->dstCpuImg, 1.0f, d->stream, nullptr));

        host = (T *)d->dstCpuImg.pixels;
        for (int plane = 0; plane < 3; plane++) {
            const size_t stride = vsapi->getStride(dst, plane);
            uint8_t *ptr = (uint8_t*)vsapi->getWritePtr(dst, plane);
            const size_t w = d->out_image_width(), h = d->out_image_height();
            for (size_t i = 0; i < h; i++)
                for (size_t j = 0; j < d->out_image_width(); j++)
                    *(T*)&ptr[i * stride + j * sizeof(T)] = host[plane * h * w + i * w + j];
        }

        vsapi->freeFrame(src);
        return dst;
    }

    return nullptr;
}

static void VS_CC vfxFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    VfxData *d = static_cast<VfxData *>(instanceData);
    vsapi->freeNode(d->node);

    delete d;
}

static void VS_CC vfxCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<VfxData> d(new VfxData);
    int err;

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

        enum { OP_AR, OP_SUPERRES, OP_DENOISE };
        const NvVFX_EffectSelector selectors[] = { NVVFX_FX_ARTIFACT_REDUCTION, NVVFX_FX_SUPER_RES, NVVFX_FX_DENOISING };
        size_t op = int64ToIntS(vsapi->propGetInt(in, "op", 0, &err));
        if (err) throw std::runtime_error("op is required argument");
        if (op >= sizeof selectors / sizeof selectors[0])
            throw std::runtime_error("op is out of range.");

        if (op != OP_SUPERRES)
            d->scale = 1;
        else {
            double scale = vsapi->propGetFloat(in, "scale", 0, &err);
            if (err) scale = 1;
            if (scale < 1)
                throw std::runtime_error("invalid scale parameter");
            d->scale = scale;
        }

        double strength = vsapi->propGetFloat(in, "strength", 0, &err);
        if (err) strength = 0;
        d->strength = strength;

        const char *modelDir = getenv("MODEL_DIR"); // TODO: configurable model directory?
        if (modelDir == nullptr)
            modelDir = "C:\\Program Files\\NVIDIA Corporation\\NVIDIA Video Effects\\models";
        fprintf(stderr, "MODEL_DIR = %s\n", modelDir);

        NvCV_Status r = NvVFX_CreateEffect(selectors[op], &d->vfx);
        if (r != NVCV_SUCCESS) {
            const char *err = NvCV_GetErrorStringFromCode(r);
            fprintf(stderr, "NvVFX_CreateEffect failed: %x (%s)\n", r, err);
            throw std::runtime_error("unable to create effect: " + std::string(err));
        }

        CK_VFX(NvVFX_CudaStreamCreate(&d->stream));
        CK_VFX(NvVFX_SetCudaStream(d->vfx, NVVFX_CUDA_STREAM, d->stream));

        if (op == OP_AR || op == OP_SUPERRES)
            r = NvVFX_SetU32(d->vfx, NVVFX_STRENGTH, int(d->strength));
        else if (op == OP_DENOISE)
            r = NvVFX_SetF32(d->vfx, NVVFX_STRENGTH, d->strength);
        else assert(false);
        if (r != NVCV_SUCCESS) {
            const char *err = NvCV_GetErrorStringFromCode(r);
            fprintf(stderr, "NvVFX set strength failed: %x (%s)\n", r, err);
            throw std::runtime_error("failed to set strength: " + std::string(err));
        }

        r = NvVFX_SetString(d->vfx, NVVFX_MODEL_DIRECTORY, modelDir);
        if (r != NVCV_SUCCESS) {
            fprintf(stderr, "NvVFX set model directory to %s failed: %x (%s)\n", modelDir, r, NvCV_GetErrorStringFromCode(r));
            throw std::runtime_error("unable to set model directory " + std::string(modelDir));
        }

        if (op == OP_DENOISE) {
            unsigned int stateSizeInBytes = 0;
            CK_VFX(NvVFX_GetU32(d->vfx, NVVFX_STATE_SIZE, &stateSizeInBytes));
            CK_CUDA(cuMemAlloc_v2(&d->state, stateSizeInBytes));
            CK_CUDA(cuMemsetD8Async(d->state, 0, stateSizeInBytes, d->stream));
            void *stateArray[1] = { d->state };
            CK_VFX(NvVFX_SetObject(d->vfx, NVVFX_STATE, (void*)stateArray));
        }
    } catch (std::runtime_error &e) {
        if (d->node)
            vsapi->freeNode(d->node);
        vsapi->setError(out, (std::string{ "DLVFX: " } + e.what()).c_str());
        return;
    }

    d->in_width = d->vi.width;
    d->in_height = d->vi.height;
    d->vi.width *= d->scale;
    d->vi.height *= d->scale;

    CK_VFX(NvCVImage_Alloc(&d->srcCpuImg, d->in_image_width(), d->in_image_height(), NVCV_RGB, NVCV_F32, NVCV_PLANAR, NVCV_CPU, 1));
    CK_VFX(NvCVImage_Alloc(&d->srcGpuImg, d->in_image_width(), d->in_image_height(), NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 1));
    CK_VFX(NvCVImage_Alloc(&d->dstCpuImg, d->out_image_width(), d->out_image_height(), NVCV_RGB, NVCV_F32, NVCV_PLANAR, NVCV_CPU, 1));
    CK_VFX(NvCVImage_Alloc(&d->dstGpuImg, d->out_image_width(), d->out_image_height(), NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 1));

    CK_VFX(NvVFX_SetImage(d->vfx, NVVFX_INPUT_IMAGE, &d->srcGpuImg));
    CK_VFX(NvVFX_SetImage(d->vfx, NVVFX_OUTPUT_IMAGE, &d->dstGpuImg));

    CK_VFX(NvVFX_Load(d->vfx));

    vsapi->createFilter(in, out, "DLVFX", vfxInit, vfxGetFrame, vfxFree, fmParallel, 0, d.release(), core);
}

//////////////////////////////////////////
// Init

#ifndef STANDALONE_VFX
void VS_CC vfxInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
#else
VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("info.akarin.plugin", "akarin2", "Experimental Nvidia Maxine plugin", VAPOURSYNTH_API_VERSION, 1, plugin);
#endif
    unsigned int version = 0;
    if (NvVFX_GetVersion(&version) == NVCV_SUCCESS)
        registerFunc("DLVFX", "clip:clip;op:int;scale:float:opt;strength:float:opt", vfxCreate, nullptr, plugin);
}
