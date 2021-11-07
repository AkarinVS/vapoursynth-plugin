#ifndef __VMAF_CAMBI_H__
#define __VMAF_CAMBI_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUM_SCALES 5
#define NUM_DIFFS 4
#ifdef CAMBI_IMPL
static const int g_scale_weights[NUM_SCALES] = {16, 8, 4, 2, 1};
static const int g_diffs_to_consider[NUM_DIFFS] = {1, 2, 3, 4};
static const int g_diffs_weights[NUM_DIFFS] = {1, 2, 3, 4};
#endif

#define PICS_BUFFER_SIZE 2

typedef struct CambiState {
    VmafPicture pics[PICS_BUFFER_SIZE];
    unsigned enc_width;
    unsigned enc_height;
    uint16_t tvi_for_diff[NUM_DIFFS];
    uint16_t window_size;
    double topk;
    double tvi_threshold;
    float *c_values;
    uint16_t *c_values_histograms;
    uint32_t *mask_dp;
} CambiState;

void cambi_config(CambiState *s);
int cambi_init(CambiState *s, unsigned w, unsigned h);
int cambi_extract(CambiState *s, VmafPicture *pic, double *score, float **c_values);
int cambi_close(CambiState *s);

static inline void scale_dimension(unsigned *width, unsigned int scale) {
    for (unsigned i = 0; i < scale; i++)
        *width = (*width + 1) >> 1;
}

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_CAMBI_H__ */

