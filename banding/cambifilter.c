#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "internalfilters.h"
#include "libvmaf/picture.h"
#include "libvmaf/cambi.h"

#include "VapourSynth.h"
#include "VSHelper.h"

typedef struct {
    VSNodeRef *node;
    VSVideoInfo vi;
    CambiState s;
    int scores;
} CambiData;

static void VS_CC cambiInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    CambiData *d = (CambiData *) *instanceData;
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC cambiGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    CambiData *d = (CambiData *) *instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        const unsigned int width = vsapi->getFrameWidth(src, 0);
        const unsigned int height = vsapi->getFrameHeight(src, 0);
        VSFrameRef *dst = vsapi->copyFrame(src, core);

        VmafPicture pic; // shares memory with src
        pic.pix_fmt = VMAF_PIX_FMT_YUV400P; // GRAY
        pic.bpc = 8;
        pic.w[0] = width;
        pic.h[0] = height;
        pic.stride[0] = vsapi->getStride(src, 0);
        pic.data[0] = (uint8_t *)vsapi->getReadPtr(src, 0);
        pic.ref = NULL;

        double score;
        CambiState s = d->s; // cambiGetFrame might be called concurrently
        int err = cambi_init(&s, pic.w[0], pic.h[0]);
        assert(err == 0);

        float *c_values[NUM_SCALES];
        if (d->scores) {
            unsigned int w = width, h = height;
            for (int i = 0; i < NUM_SCALES; i++) {
                c_values[i] = calloc(w * h, sizeof *c_values[i]);
                scale_dimension(&w, 1);
                scale_dimension(&h, 1);
            }
        }
        err = cambi_extract(&s, &pic, &score, d->scores ? c_values : NULL);
        cambi_close(&s);
        vsapi->freeFrame(src);

        VSMap *prop = vsapi->getFramePropsRW(dst);
        if (d->scores) {
            const VSFormat *grays = vsapi->getFormatPreset(pfGrayS, core);
            unsigned int w = width, h = height;
            for (int i = 0; i < NUM_SCALES; i++) {
                VSFrameRef *f = vsapi->newVideoFrame(grays, w, h, src, core);
                const int row_size = w * sizeof(float);
                vs_bitblt(vsapi->getWritePtr(f, 0), vsapi->getStride(f, 0), c_values[i], row_size, row_size, h);
                free(c_values[i]);
                scale_dimension(&w, 1);
                scale_dimension(&h, 1);
                char name[16];
                sprintf(name, "CAMBI_SCALE%d", i);
                vsapi->propSetFrame(prop, name, f, paReplace);
            }
        }
        assert(err == 0);

        err = vsapi->propSetFloat(prop, "CAMBI", score, paReplace);
        assert(err == 0);

        return dst;
    }

    return NULL;
}

static void VS_CC cambiFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    CambiData *d = (CambiData *)instanceData;
    vsapi->freeNode(d->node);
    cambi_close(&d->s);
    free(d);
}

// This function is responsible for validating arguments and creating a new filter
static void VS_CC cambiCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    CambiData d;

    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = *vsapi->getVideoInfo(d.node);

    if (!isConstantFormat(&d.vi) || d.vi.format->sampleType != stInteger || d.vi.format->bitsPerSample != 8) {
        vsapi->setError(out, "Cambi: only constant format with 8bit integer samples supported");
        vsapi->freeNode(d.node);
        return;
    }

    cambi_config(&d.s);
#define GETARG(type, var, name, api, min, max) \
    do { \
        int err; \
        type x = vsapi->api(in, #name, 0, &err); \
        if (err != 0) break; \
        if (x < min || x > max) { \
            char errmsg[256]; \
            snprintf(errmsg, sizeof errmsg, "Cambi: argument %s=%f is out of range [%f,%f] (default=%f)", #name, (double)x, (double)min, (double)max, (double)var.name); \
            vsapi->setError(out, errmsg); \
            vsapi->freeNode(d.node); \
            return; \
        } \
        var.name = x; \
    } while (0)
    GETARG(int, d.s, window_size, propGetInt, 15, 127);
    GETARG(double, d.s, topk, propGetFloat, 0.0001, 1);
    GETARG(double, d.s, tvi_threshold, propGetFloat, 0.0001, 1);
    d.scores = 0;
    GETARG(int, d, scores, propGetInt, 0, 1);

    int err = cambi_init(&d.s, d.vi.width, d.vi.height);
    if (err != 0) {
        vsapi->setError(out, "cambi_init failure");
        vsapi->freeNode(d.node);
        return;
    }

    CambiData *data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Cambi", cambiInit, cambiGetFrame, cambiFree, fmParallel, 0, data, core);
}

void bandingInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    registerFunc("Cambi", "clip:clip;window_size:int:opt;topk:float:opt;tvi_threshold:float:opt;scores:int:opt;", cambiCreate, 0, plugin);
}
