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

    int num_streams;

    VSNodeRef *node;
    VSVideoInfo vi;
    double scale;
    double strength;

    int in_width, in_height;

    NvVFX_Handle vfx;
    CUstream stream;
    CUdeviceptr state;

    NvCVImage srcGpuImg;
    NvCVImage dstGpuImg;
    NvCVImage srcTmpImg, dstTmpImg;
    void *srcCpuBuf, *dstCpuBuf;

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
        NvCVImage_Dealloc(&dstGpuImg);
        NvCVImage_Dealloc(&srcTmpImg);
        NvCVImage_Dealloc(&dstTmpImg);
        cuMemFreeHost(srcCpuBuf);
        cuMemFreeHost(dstCpuBuf);
    }
};

static void VS_CC vfxInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    VfxData *d = static_cast<VfxData *>(*instanceData);
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC vfxGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    VfxData *ds = static_cast<VfxData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, ds->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        for (int i = 0; i < ds->num_streams; ++i) {
            auto d = ds + i;
            std::unique_lock<std::mutex> lock(d->lock, std::defer_lock);
            if (i + 1 < ds->num_streams) {
                if (!lock.try_lock()) continue;
            } else {
                lock.lock();
            }

            const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);

            const VSFormat *fi = d->vi.format;
            assert(vsapi->getFrameHeight(src, 0) == (int)d->in_image_height());
            assert(vsapi->getFrameWidth(src, 0) == (int)d->in_image_width());
            int planes[3] = { 0, 1, 2 };
            const VSFrameRef *srcf[3] = { nullptr, nullptr, nullptr };
            VSFrameRef *dst = vsapi->newVideoFrame2(fi, d->out_image_width(), d->out_image_height(), srcf, planes, src, core);

            auto host = static_cast<char*>(d->srcCpuBuf);
            for (int plane = 0; plane < 3; plane++) {
                const auto stride = vsapi->getStride(src, plane);
                const auto *ptr = vsapi->getReadPtr(src, plane);
                const size_t w = d->in_image_width(), h = d->in_image_height();
                const auto pitch = d->srcTmpImg.pitch;
                vs_bitblt(host + pitch * h * plane, pitch, ptr, stride, w * d->vi.format->bytesPerSample, h);
            }

            {
                CUDA_MEMCPY2D mcp2d = {
                    .srcXInBytes = 0,
                    .srcY = 0,
                    .srcMemoryType = CU_MEMORYTYPE_HOST,
                    .srcHost = host,
                    .srcPitch = (size_t)d->srcTmpImg.pitch,
                    .dstXInBytes = 0,
                    .dstY = 0,
                    .dstMemoryType = CU_MEMORYTYPE_DEVICE,
                    .dstDevice = (CUdeviceptr)d->srcTmpImg.pixels,
                    .dstPitch = (size_t)d->srcTmpImg.pitch,
                    .WidthInBytes = (size_t)d->in_image_width() * d->vi.format->bytesPerSample,
                    .Height = d->in_image_height() * 3
                };
                CK_CUDA(cuMemcpy2DAsync_v2(&mcp2d, d->stream));
            }

            CK_VFX(NvCVImage_Transfer(&d->srcTmpImg, &d->srcGpuImg, 1.0f, d->stream, nullptr));
            CK_VFX(NvVFX_Run(d->vfx, 1));
            CK_VFX(NvCVImage_Transfer(&d->dstGpuImg, &d->dstTmpImg, 1.0f, d->stream, nullptr));

            host = static_cast<char*>(d->dstCpuBuf);
            {
                CUDA_MEMCPY2D mcp2d = {
                    .srcXInBytes = 0,
                    .srcY = 0,
                    .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                    .srcDevice = (CUdeviceptr)d->dstTmpImg.pixels,
                    .srcPitch = (size_t)d->dstTmpImg.pitch,
                    .dstXInBytes = 0,
                    .dstY = 0,
                    .dstMemoryType = CU_MEMORYTYPE_HOST,
                    .dstHost = host,
                    .dstPitch = (size_t)d->dstTmpImg.pitch,
                    .WidthInBytes = (size_t)d->out_image_width() * d->vi.format->bytesPerSample,
                    .Height = d->out_image_height() * 3
                };
                CK_CUDA(cuMemcpy2DAsync_v2(&mcp2d, d->stream));
            }

            CK_CUDA(cuStreamSynchronize(d->stream));

            for (int plane = 0; plane < 3; plane++) {
                const auto stride = vsapi->getStride(dst, plane);
                auto *ptr = vsapi->getWritePtr(dst, plane);
                const size_t w = d->out_image_width(), h = d->out_image_height();
                const auto pitch = d->dstTmpImg.pitch;
                vs_bitblt(ptr, stride, host + pitch * h * plane, pitch, w * d->vi.format->bytesPerSample, h);
            }

            vsapi->freeFrame(src);
            lock.unlock();
            return dst;
        }
    }

    return nullptr;
}

static void VS_CC vfxFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    VfxData *ds = static_cast<VfxData *>(instanceData);
    for (int i = 0; i < ds->num_streams; ++i) {
        auto d = ds + i;
        vsapi->freeNode(d->node);
    }

    delete[] ds;
}

static void VS_CC vfxCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    int err;
    auto num_streams = int64ToIntS(vsapi->propGetInt(in, "num_streams", 0, &err));
    if (err) num_streams = 1;

    std::unique_ptr<VfxData[]> ds(new VfxData[num_streams]);

    for (int i = 0; i < num_streams; ++i) {
        auto d = &ds[i];
        d->num_streams = num_streams;
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

        NvCVImage_ComponentType src_ct, dst_ct;
        if (auto bps = d->vi.format->bitsPerSample, st = d->vi.format->sampleType; bps == 32 && st == stFloat) {
            src_ct = NVCV_F32;
        } else if (bps == 8 && st == stInteger) {
            src_ct = NVCV_U8;
        } else {
            throw std::runtime_error("unsupported clip format");
        }

        int output_depth = int64ToIntS(vsapi->propGetInt(in, "output_depth", 0, &err));
        if (err) output_depth = d->vi.format->bitsPerSample;
        if (output_depth == 32) {
            dst_ct = NVCV_F32;
        } else if (output_depth == 8) {
            dst_ct = NVCV_U8;
        }

        CK_VFX(NvCVImage_Alloc(&d->srcTmpImg, d->in_image_width(), d->in_image_height(), NVCV_RGB, src_ct, NVCV_PLANAR, NVCV_GPU, 0));
        CK_VFX(NvCVImage_Alloc(&d->srcGpuImg, d->in_image_width(), d->in_image_height(), NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 0));
        CK_VFX(NvCVImage_Alloc(&d->dstTmpImg, d->out_image_width(), d->out_image_height(), NVCV_RGB, dst_ct, NVCV_PLANAR, NVCV_GPU, 0));
        CK_VFX(NvCVImage_Alloc(&d->dstGpuImg, d->out_image_width(), d->out_image_height(), NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 0));

        CK_CUDA(cuMemHostAlloc(&d->srcCpuBuf, d->srcTmpImg.pitch * d->in_image_height() * 3, CU_MEMHOSTALLOC_WRITECOMBINED));
        CK_CUDA(cuMemHostAlloc(&d->dstCpuBuf, d->dstTmpImg.pitch * d->out_image_height() * 3, 0));

        CK_VFX(NvVFX_SetImage(d->vfx, NVVFX_INPUT_IMAGE, &d->srcGpuImg));
        CK_VFX(NvVFX_SetImage(d->vfx, NVVFX_OUTPUT_IMAGE, &d->dstGpuImg));

        CK_VFX(NvVFX_Load(d->vfx));
    }

    vsapi->createFilter(in, out, "DLVFX", vfxInit, vfxGetFrame, vfxFree, fmParallel, 0, ds.release(), core);
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
        registerFunc("DLVFX", "clip:clip;op:int;scale:float:opt;strength:float:opt;output_depth:int:opt;num_streams:int:opt", vfxCreate, nullptr, plugin);
}
