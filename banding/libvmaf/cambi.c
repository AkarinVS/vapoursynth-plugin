/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common/macros.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "mem.h"
#include "picture.h"

#define CAMBI_IMPL
#include "cambi.h"

/* Ratio of pixels for computation, must be 0 > topk >= 1.0 */
#define DEFAULT_CAMBI_TOPK_POOLING (0.6)

/* Window size to compute CAMBI: 63 corresponds to approximately 1 degree at 4k scale */
#define DEFAULT_CAMBI_WINDOW_SIZE (63)

/* Visibilty threshold for luminance ΔL < tvi_threshold*L_mean for BT.1886 */
#define DEFAULT_CAMBI_TVI (0.019)

#define CAMBI_MIN_WIDTH (320)
#define CAMBI_MAX_WIDTH (4096)
#define CAMBI_4K_WIDTH (3840)
#define CAMBI_4K_HEIGHT (2160)

#define NUM_ALL_DIFFS (2 * NUM_DIFFS + 1)
static const int g_all_diffs[NUM_ALL_DIFFS] = {-4, -3, -2, -1, 0, 1, 2, 3, 4};
static const uint16_t g_c_value_histogram_offset = 4; // = -g_all_diffs[0]

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define SWAP_FLOATS(x, y) \
    {                     \
        float temp = x;   \
        x = y;            \
        y = temp;         \
    }
#define SWAP_PICS(x, y)      \
    {                        \
        uint16_t *temp = x;  \
        x = y;               \
        y = temp;            \
    }

#define MASK_FILTER_SIZE 7

static const VmafOption options[] = {
    {
        .name = "enc_width",
        .help = "Encoding width",
        .offset = offsetof(CambiState, enc_width),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 320,
        .max = 7680,
    },
    {
        .name = "enc_height",
        .help = "Encoding height",
        .offset = offsetof(CambiState, enc_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 200,
        .max = 4320,
    },
    {
        .name = "window_size",
        .help = "Window size to compute CAMBI: 63 corresponds to ~1 degree at 4k",
        .offset = offsetof(CambiState, window_size),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_CAMBI_WINDOW_SIZE,
        .min = 15,
        .max = 127,
    },
    {
        .name = "topk",
        .help = "Ratio of pixels for the spatial pooling computation, must be 0 > topk >= 1.0",
        .offset = offsetof(CambiState, topk),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_CAMBI_TOPK_POOLING,
        .min = 0.0001,
        .max = 1.0,
    },
    {
        .name = "tvi_threshold",
        .help = "Visibilty threshold for luminance ΔL < tvi_threshold*L_mean for BT.1886",
        .offset = offsetof(CambiState, tvi_threshold),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_CAMBI_TVI,
        .min = 0.0001,
        .max = 1.0,
    },
    { 0 }
};

/* Visibility threshold functions */
#define BT1886_GAMMA (2.4)

enum CambiTVIBisectFlag {
    CAMBI_TVI_BISECT_TOO_SMALL,
    CAMBI_TVI_BISECT_CORRECT,
    CAMBI_TVI_BISECT_TOO_BIG
};

static FORCE_INLINE inline int clip(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

static FORCE_INLINE inline double bt1886_eotf(double V, double gamma, double Lw, double Lb) {
    double a = pow(pow(Lw, 1.0 / gamma) - pow(Lb, 1.0 / gamma), gamma);
    double b = pow(Lb, 1.0 / gamma) / (pow(Lw, 1.0 / gamma) - pow(Lb, 1.0 / gamma));
    double L = a * pow(MAX(V + b, 0), gamma);
    return L;
}

static FORCE_INLINE inline void range_foot_head(int bitdepth, const char *pix_range,
                                                int *foot, int *head) {
    int foot_8b = 0;
    int head_8b = 255;
    if (!strcmp(pix_range, "standard")) {
        foot_8b = 16;
        head_8b = 235;
    }
    *foot = foot_8b * (pow(2, bitdepth - 8));
    *head = head_8b * (pow(2, bitdepth - 8));
}

static double normalize_range(int sample, int bitdepth, const char *pix_range) {
    int foot, head, clipped_sample;
    range_foot_head(bitdepth, pix_range, &foot, &head);
    clipped_sample = clip(sample, foot, head);
    return (double)(clipped_sample - foot) / (head - foot);
}

static double luminance_bt1886(int sample, int bitdepth,
                               double Lw, double Lb, const char *pix_range) {
    double normalized;
    normalized = normalize_range(sample, bitdepth, pix_range);
    return bt1886_eotf(normalized, BT1886_GAMMA, Lw, Lb);
}

static bool tvi_condition(int sample, int diff, double tvi_threshold,
                          int bitdepth, double Lw, double Lb, const char *pix_range) {
    double mean_luminance = luminance_bt1886(sample, bitdepth, Lw, Lb, pix_range);
    double diff_luminance = luminance_bt1886(sample + diff, bitdepth, Lw, Lb, pix_range);
    double delta_luminance = diff_luminance - mean_luminance;
    return (delta_luminance > tvi_threshold * mean_luminance);
}

static enum CambiTVIBisectFlag tvi_hard_threshold_condition(int sample, int diff,
                                                            double tvi_threshold,
                                                            int bitdepth, double Lw, double Lb,
                                                            const char *pix_range) {
    bool condition;
    condition = tvi_condition(sample, diff, tvi_threshold, bitdepth, Lw, Lb, pix_range);
    if (!condition) return CAMBI_TVI_BISECT_TOO_BIG;

    condition = tvi_condition(sample + 1, diff, tvi_threshold, bitdepth, Lw, Lb, pix_range);
    if (condition) return CAMBI_TVI_BISECT_TOO_SMALL;

    return CAMBI_TVI_BISECT_CORRECT;
}

static int get_tvi_for_diff(int diff, double tvi_threshold, int bitdepth,
                            double Lw, double Lb, const char *pix_range) {
    int foot, head, mid;
    enum CambiTVIBisectFlag tvi_bisect;
    const int max_val = (1 << bitdepth) - 1;

    range_foot_head(bitdepth, pix_range, &foot, &head);
    head = head - diff - 1;

    tvi_bisect = tvi_hard_threshold_condition(foot, diff, tvi_threshold, bitdepth,
                                              Lw, Lb, pix_range);
    if (tvi_bisect == CAMBI_TVI_BISECT_TOO_BIG) return 0;
    if (tvi_bisect == CAMBI_TVI_BISECT_CORRECT) return foot;

    tvi_bisect = tvi_hard_threshold_condition(head, diff, tvi_threshold, bitdepth,
                                              Lw, Lb, pix_range);
    if (tvi_bisect == CAMBI_TVI_BISECT_TOO_SMALL) return max_val;
    if (tvi_bisect == CAMBI_TVI_BISECT_CORRECT) return head;

    // bisect
    while (1) {
        mid = foot + (head - foot) / 2;
        tvi_bisect = tvi_hard_threshold_condition(mid, diff, tvi_threshold, bitdepth,
                                                  Lw, Lb, pix_range);
        if (tvi_bisect == CAMBI_TVI_BISECT_TOO_BIG)
            head = mid;
        else if (tvi_bisect == CAMBI_TVI_BISECT_TOO_SMALL)
            foot = mid;
        else if (tvi_bisect == CAMBI_TVI_BISECT_CORRECT)
            return mid;
        else // Should never get here (todo: add assert)
            (void)0;
    }
}

static FORCE_INLINE inline void adjust_window_size(uint16_t *window_size, unsigned input_width) {
    (*window_size) = ((*window_size) * input_width) / CAMBI_4K_WIDTH;
}

void cambi_config(CambiState *s)
{
    memset(s, 0, sizeof *s);
    s->window_size = DEFAULT_CAMBI_WINDOW_SIZE;
    s->topk = DEFAULT_CAMBI_TOPK_POOLING;
    s->tvi_threshold = DEFAULT_CAMBI_TVI;
}

int cambi_init(CambiState *s, unsigned w, unsigned h)
{
    if (s->enc_width == 0 || s->enc_height == 0) {
        s->enc_width = w;
        s->enc_height = h;
    }

    w = s->enc_width;
    h = s->enc_height;

    if (w < CAMBI_MIN_WIDTH || w > CAMBI_MAX_WIDTH)
        return -EINVAL;
    int err = 0;
    for (unsigned i = 0; i < PICS_BUFFER_SIZE; i++)
        err |= vmaf_picture_alloc(&s->pics[i], VMAF_PIX_FMT_YUV400P, 10, w, h);

    for (int d = 0; d < NUM_DIFFS; d++) {
        // BT1886 parameters
        s->tvi_for_diff[d] = get_tvi_for_diff(g_diffs_to_consider[d], s->tvi_threshold, 10,
                                              300.0, 0.01, "standard");
        s->tvi_for_diff[d] += g_c_value_histogram_offset;
    }

    adjust_window_size(&s->window_size, w);
    s->c_values = aligned_malloc(ALIGN_CEIL(w * sizeof(float)) * h, 32);

    const uint16_t num_bins = 1024 + (g_all_diffs[NUM_ALL_DIFFS - 1] - g_all_diffs[0]);
    s->c_values_histograms = aligned_malloc(ALIGN_CEIL(w * num_bins * sizeof(uint16_t)), 32);

    int pad_size = MASK_FILTER_SIZE >> 1;
    int dp_width = w + 2 * pad_size + 1;
    int dp_height = 2 * pad_size + 2;
    s->mask_dp = aligned_malloc(ALIGN_CEIL(dp_height * dp_width * sizeof(uint32_t)), 32);

    return err;
}

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                unsigned bpc, unsigned w, unsigned h) {
    (void) pix_fmt;
    (void) bpc;

    CambiState *s = fex->priv;

    return cambi_init(s, w, h);
}

/* Preprocessing functions */
static void decimate_generic_10b(const VmafPicture *pic, VmafPicture *out_pic) {
    uint16_t *data = pic->data[0];
    uint16_t *out_data = out_pic->data[0];
    ptrdiff_t stride = pic->stride[0] >> 1;
    ptrdiff_t out_stride = out_pic->stride[0] >> 1;
    unsigned in_w = pic->w[0];
    unsigned in_h = pic->h[0];
    unsigned out_w = out_pic->w[0];
    unsigned out_h = out_pic->h[0];

    // if the input and output sizes are the same
    if (in_w == out_w && in_h == out_h){
        memcpy(out_data, data, stride * pic->h[0] * sizeof(uint16_t));
        return;
    }

    float ratio_x = (float)in_w / out_w;
    float ratio_y = (float)in_h / out_h;

    float start_x = ratio_x / 2 - 0.5;
    float start_y = ratio_y / 2 - 0.5;

    float y = start_y;
    for (unsigned i = 0; i < out_h; i++) {
        unsigned ori_y = (int)(y + 0.5);
        float x = start_x;
        for (unsigned j = 0; j < out_w; j++) {
            unsigned ori_x = (int)(x + 0.5);
            out_data[i * out_stride + j] = data[ori_y * stride + ori_x];
            x += ratio_x;
        }
        y += ratio_y;
    }
}

static void decimate_generic_8b_and_convert_to_10b(const VmafPicture *pic, VmafPicture *out_pic) {
    uint8_t *data = pic->data[0];
    uint16_t *out_data = out_pic->data[0];
    ptrdiff_t stride = pic->stride[0];
    ptrdiff_t out_stride = out_pic->stride[0] >> 1;
    unsigned in_w = pic->w[0];
    unsigned in_h = pic->h[0];
    unsigned out_w = out_pic->w[0];
    unsigned out_h = out_pic->h[0];

    // if the input and output sizes are the same
    if (in_w == out_w && in_h == out_h) {
        for (unsigned i = 0; i < out_h; i++)
            for (unsigned j = 0; j < out_w; j++)
                out_data[i * out_stride + j] = data[i * stride + j] << 2;
        return;
    }

    float ratio_x = (float)in_w / out_w;
    float ratio_y = (float)in_h / out_h;

    float start_x = ratio_x / 2 - 0.5;
    float start_y = ratio_y / 2 - 0.5;

    float y = start_y;
    for (unsigned i = 0; i < out_h; i++) {
        unsigned ori_y = (int)(y + 0.5);
        float x = start_x;
        for (unsigned j = 0; j < out_w; j++) {
            unsigned ori_x = (int)(x + 0.5);
            out_data[i * out_stride + j] = data[ori_y * stride + ori_x] << 2;
            x += ratio_x;
        }
        y += ratio_y;
    }
}

static void anti_dithering_filter(VmafPicture *pic) {
    uint16_t *data = pic->data[0];
    int stride = pic->stride[0] >> 1;

    for (unsigned i = 0; i < pic->h[0] - 1; i++) {
        for (unsigned j = 0; j < pic->w[0] - 1; j++) {
            data[i * stride + j] = (data[i * stride + j] +
                                    data[i * stride + j + 1] +
                                    data[(i + 1) * stride + j] +
                                    data[(i + 1) * stride + j + 1]) >> 2;
        }

        // Last column
        unsigned j = pic->w[0] - 1;
        data[i * stride + j] = (data[i * stride + j] +
                                data[(i + 1) * stride + j]) >> 1;
    }

    // Last row
    unsigned i = pic->h[0] - 1;
    for (unsigned j = 0; j < pic->w[0] - 1; j++) {
        data[i * stride + j] = (data[i * stride + j] +
                                data[i * stride + j + 1]) >> 1;
    }
}

int cambi_preprocessing(const VmafPicture *image, VmafPicture *preprocessed) {
    if (image->bpc == 8) {
        decimate_generic_8b_and_convert_to_10b(image, preprocessed);
        anti_dithering_filter(preprocessed);
    }
    else {
        decimate_generic_10b(image, preprocessed);
    }

    return 0;
}

/* Banding detection functions */
static void decimate(VmafPicture *image, unsigned width, unsigned height) {
    uint16_t *data = image->data[0];
    ptrdiff_t stride = image->stride[0] >> 1;
    for (unsigned i = 0; i < height; i++) {
        for (unsigned j = 0; j < width; j++) {
            data[i * stride + j] = data[(i << 1) * stride + (j << 1)];
        }
    }
}

static FORCE_INLINE inline uint16_t mode_selection(uint16_t *elems, uint8_t *hist) {
    unsigned max_counts = 0;
    uint16_t max_mode = 1024;
    // Set the 9 entries to 0
    for (int i = 0; i < 9; i++) {
        hist[elems[i]] = 0;
    }
    // Increment the 9 entries and find the mode
    for (int i = 0; i < 9; i++) {
        uint16_t value = elems[i];
        hist[value]++;
        uint8_t count = hist[value];
        if (count >= 5) {
            return value;
        }
        if (count > max_counts || (count == max_counts && value < max_mode)) {
            max_counts = count;
            max_mode = value;
        }
    }
    return max_mode;
}

static void filter_mode(const VmafPicture *image, int width, int height) {
    uint16_t *data = image->data[0];
    ptrdiff_t stride = image->stride[0] >> 1;
    uint16_t curr[9];
    uint8_t *hist = malloc(1024 * sizeof(uint8_t));
    uint16_t *buffer = malloc(3 * width * sizeof(uint16_t));
    for (int i = 0; i < height + 2; i++) {
        if (i < height) {
            for (int j = 0; j < width; j++) {
                // Get the 9 elements into an array for cache optimization
                for (int row = 0; row < 3; row++) {
                    for (int col = 0; col < 3; col++) {
                        int clamped_row = CLAMP(i + row - 1, 0, height - 1);
                        int clamped_col = CLAMP(j + col - 1, 0, width - 1);
                        curr[3 * row + col] = data[clamped_row * stride + clamped_col];
                    }
                }
                buffer[(i % 3) * width + j] = mode_selection(curr, hist);
            }
        }
        if (i >= 2) {
            uint16_t *dest = data + (i - 2) * stride;
            uint16_t *src = buffer + ((i + 1) % 3) * width;
            memcpy(dest, src, width * sizeof(uint16_t));
        }
    }

    free(hist);
    free(buffer);
}

static FORCE_INLINE inline uint16_t get_mask_index(unsigned input_width, unsigned input_height,
                                                   uint16_t filter_size) {
    const int slope = 3;
    double resolution_ratio = sqrt((CAMBI_4K_WIDTH * CAMBI_4K_HEIGHT) / (input_width * input_height));

    return (uint16_t)(floor(pow(filter_size, 2) / 2) - slope * (resolution_ratio - 1));
}

static FORCE_INLINE inline bool get_derivative_data(const uint16_t *data, int width, int height, int i, int j, ptrdiff_t stride) {
    return (i == height - 1 || (data[i * stride + j] == data[(i + 1) * stride + j])) &&
           (j == width - 1 || (data[i * stride + j] == data[i * stride + j + 1]));
}

/*
* This function calculates the horizontal and vertical derivatives of the image using 2x1 and 1x2 kernels.
* We say a pixel has zero_derivative=1 if it's equal to its right and bottom neighbours, and =0 otherwise (edges also count as "equal").
* This function then computes the sum of zero_derivative on the filter_size x filter_size square around each pixel
* and stores 1 into the corresponding mask index iff this number is larger than mask_index.
* To calculate the square sums, it uses a dynamic programming algorithm based on inclusion-exclusion.
* To save memory, it uses a DP matrix of only the necessary size, rather than the full matrix, and indexes its rows cyclically.
*/
static void get_spatial_mask_for_index(const VmafPicture *image, VmafPicture *mask,
                                       uint32_t *dp, uint16_t mask_index, uint16_t filter_size,
                                       int width, int height) {
    uint16_t pad_size = filter_size >> 1;
    uint16_t *image_data = image->data[0];
    uint16_t *mask_data = mask->data[0];
    ptrdiff_t stride = image->stride[0] >> 1;

    int dp_width = width + 2 * pad_size + 1;
    int dp_height = 2 * pad_size + 2;
    memset(dp, 0, dp_width * dp_height * sizeof(uint32_t));

    // Initial computation: fill dp except for the last row
    for (int i = 0; i < pad_size; i++) {
        for (int j = 0; j < width + pad_size; j++) {
            int value = (i < height && j < width ? get_derivative_data(image_data, width, height, i, j, stride) : 0);
            int curr_row = i + pad_size + 1;
            int curr_col = j + pad_size + 1;
            dp[curr_row * dp_width + curr_col] =
                value
                + dp[(curr_row - 1) * dp_width + curr_col]
                + dp[curr_row * dp_width + curr_col - 1]
                - dp[(curr_row - 1) * dp_width + curr_col - 1];
        }
    }

    // Start from the last row in the dp matrix
    int curr_row = dp_height - 1;
    int curr_compute = pad_size + 1;
    for (int i = pad_size; i < height + pad_size; i++) {
        // First compute the values of dp for curr_row
        for (int j = 0; j < width + pad_size; j++) {
            int value = (i < height && j < width ? get_derivative_data(image_data, width, height, i, j, stride) : 0);
            int curr_col = j + pad_size + 1;
            int prev_row = (curr_row + dp_height - 1) % dp_height;
            dp[curr_row * dp_width + curr_col] =
                value
                + dp[prev_row * dp_width + curr_col]
                + dp[curr_row * dp_width + curr_col - 1]
                - dp[prev_row * dp_width + curr_col - 1];
        }
        curr_row = (curr_row + 1) % dp_height;

        // Then use the values to compute the square sum for the curr_compute row.
        for (int j = 0; j < width; j++) {
            int curr_col = j + pad_size + 1;
            int bottom = (curr_compute + pad_size) % dp_height;
            int top = (curr_compute + dp_height - pad_size - 1) % dp_height;
            int right = curr_col + pad_size;
            int left = curr_col - pad_size - 1;
            int result =
                dp[bottom * dp_width + right]
                - dp[bottom * dp_width + left]
                - dp[top * dp_width + right]
                + dp[top * dp_width + left];
            mask_data[(i - pad_size) * stride + j] = (result > mask_index);
        }
        curr_compute = (curr_compute + 1) % dp_height;
    }
}

static void get_spatial_mask(const VmafPicture *image, VmafPicture *mask,
                             uint32_t *dp, unsigned width, unsigned height) {
    unsigned input_width = image->w[0];
    unsigned input_height = image->h[0];
    uint16_t mask_index = get_mask_index(input_width, input_height, MASK_FILTER_SIZE);
    get_spatial_mask_for_index(image, mask, dp, mask_index, MASK_FILTER_SIZE, width, height);
}

static float c_value_pixel(const uint16_t *histograms, uint16_t value, const int *diff_weights,
                           const int *diffs, uint16_t num_diffs, const uint16_t *tvi_thresholds, int histogram_col, int histogram_width) {
    uint16_t p_0 = histograms[value * histogram_width + histogram_col];
    float val, c_value = 0.0;
    for (uint16_t d = 0; d < num_diffs; d++) {
        if (value <= tvi_thresholds[d]) {
            uint16_t p_1 = histograms[(value + diffs[num_diffs + d + 1]) * histogram_width + histogram_col];
            uint16_t p_2 = histograms[(value + diffs[num_diffs - d - 1]) * histogram_width + histogram_col];
            if (p_1 > p_2) {
                val = (float)(diff_weights[d] * p_0 * p_1) / (p_1 + p_0);
            }
            else {
                val = (float)(diff_weights[d] * p_0 * p_2) / (p_2 + p_0);
            }

            if (val > c_value) {
                c_value = val;
            }
        }
    }

    return c_value;
}

static FORCE_INLINE inline void update_histogram_subtract(uint16_t *histograms, uint16_t *image, uint16_t *mask,
                                                          int i, int j, int width, ptrdiff_t stride, uint16_t pad_size) {
    uint16_t mask_val = mask[(i - pad_size - 1) * stride + j];
    if (mask_val) {
        uint16_t val = image[(i - pad_size - 1) * stride + j] + g_c_value_histogram_offset;
        for (int col = MAX(j - pad_size, 0); col < MIN(j + pad_size + 1, width); col++) {
            histograms[val * width + col]--;
        }
    }
}

static FORCE_INLINE inline void update_histogram_add(uint16_t *histograms, uint16_t *image, uint16_t *mask,
                                                     int i, int j, int width, ptrdiff_t stride, uint16_t pad_size) {
    uint16_t mask_val = mask[(i + pad_size) * stride + j];
    if (mask_val) {
        uint16_t val = image[(i + pad_size) * stride + j] + g_c_value_histogram_offset;
        for (int col = MAX(j - pad_size, 0); col < MIN(j + pad_size + 1, width); col++) {
            histograms[val * width + col]++;
        }
    }
}

static FORCE_INLINE inline void calculate_c_values_row(float *c_values, uint16_t *histograms, uint16_t *image,
                                                       uint16_t *mask, int row, int width, ptrdiff_t stride,
                                                       const uint16_t *tvi_for_diff) {
    for (int col = 0; col < width; col++) {
        if (mask[row * stride + col]) {
            c_values[row * width + col] = c_value_pixel(
                histograms, image[row * stride + col] + g_c_value_histogram_offset, g_diffs_weights, g_all_diffs, NUM_DIFFS, tvi_for_diff, col, width
            );
        }
    }
}

static void calculate_c_values(VmafPicture *pic, const VmafPicture *mask_pic,
                               float *c_values, uint16_t *histograms, uint16_t window_size,
                               const uint16_t *tvi_for_diff, int width, int height) {
    uint16_t pad_size = window_size >> 1;
    const uint16_t num_bins = 1024 + (g_all_diffs[NUM_ALL_DIFFS - 1] - g_all_diffs[0]);

    uint16_t *image = pic->data[0];
    uint16_t *mask = mask_pic->data[0];
    ptrdiff_t stride = pic->stride[0] >> 1;

    memset(c_values, 0.0, sizeof(float) * width * height);

    // Use a histogram for each pixel in width
    // histograms[i * width + j] accesses the j'th histogram, i'th value
    // This is done for cache optimization reasons
    memset(histograms, 0, width * num_bins * sizeof(uint16_t));

    // First pass: first pad_size rows
    for (int i = 0; i < pad_size; i++) {
        for (int j = 0; j < width; j++) {
            uint16_t mask_val = mask[i * stride + j];
            if (mask_val) {
                uint16_t val = image[i * stride + j] + g_c_value_histogram_offset;
                for (int col = MAX(j - pad_size, 0); col < MIN(j + pad_size + 1, width); col++) {
                    histograms[val * width + col]++;
                }
            }
        }
    }

    // Iterate over all rows, unrolled into 3 loops to avoid conditions
    for (int i = 0; i < pad_size + 1; i++) {
        if (i + pad_size < height) {
            for (int j = 0; j < width; j++) {
                update_histogram_add(histograms, image, mask, i, j, width, stride, pad_size);
            }
        }
        calculate_c_values_row(c_values, histograms, image, mask, i, width, stride, tvi_for_diff);
    }
    for (int i = pad_size + 1; i < height - pad_size; i++) {
        for (int j = 0; j < width; j++) {
            update_histogram_subtract(histograms, image, mask, i, j, width, stride, pad_size);
            update_histogram_add(histograms, image, mask, i, j, width, stride, pad_size);
        }
        calculate_c_values_row(c_values, histograms, image, mask, i, width, stride, tvi_for_diff);
    }
    for (int i = height - pad_size; i < height; i++) {
        if (i - pad_size - 1 >= 0) {
            for (int j = 0; j < width; j++) {
                update_histogram_subtract(histograms, image, mask, i, j, width, stride, pad_size);
            }
        }
        calculate_c_values_row(c_values, histograms, image, mask, i, width, stride, tvi_for_diff);
    }
}

static double average_topk_elements(const float *arr, int topk_elements) {
    double sum = 0;
    for (int i = 0; i < topk_elements; i++)
        sum += arr[i];

    return (double)sum / topk_elements;
}

static void quick_select(float *arr, int n, int k) {
    int left = 0;
    int right = n - 1;
    while (left < right) {
        float pivot = arr[k];
        int i = left;
        int j = right;
        do {
            while (arr[i] > pivot) {
                i++;
            }
            while (arr[j] < pivot) {
                j--;
            }
            if (i <= j) {
                SWAP_FLOATS(arr[i], arr[j]);
                i++;
                j--;
            }
        } while (i <= j);
        if (j < k) {
            left = i;
        }
        if (k < i) {
            right = j;
        }
    }
}

static double spatial_pooling(float *c_values, double topk, unsigned width, unsigned height) {
    int num_elements = height * width;
    int topk_num_elements = clip(topk * num_elements, 1, num_elements);
    quick_select(c_values, num_elements, topk_num_elements);
    return average_topk_elements(c_values, topk_num_elements);
}

static FORCE_INLINE inline uint16_t get_pixels_in_window(uint16_t window_length) {
    return (uint16_t)pow(2 * (window_length >> 1) + 1, 2);
}

// Inner product weighting scores for each scale
static FORCE_INLINE inline double weight_scores_per_scale(double *scores_per_scale, uint16_t normalization) {
    double score = 0.0;
    for (unsigned scale = 0; scale < NUM_SCALES; scale++)
        score += (scores_per_scale[scale] * g_scale_weights[scale]);

    return score / normalization;
}

static int cambi_score(VmafPicture *pics, uint32_t *mask_dp, uint16_t window_size, double topk,
                       const uint16_t *tvi_for_diff, float *c_values, uint16_t *c_values_histograms, double *score,
                       float **c_values_ret) {
    double scores_per_scale[NUM_SCALES];
    VmafPicture *image = &pics[0];
    VmafPicture *mask = &pics[1];

    unsigned scaled_width = image->w[0];
    unsigned scaled_height = image->h[0];
    for (unsigned scale = 0; scale < NUM_SCALES; scale++) {
        if (scale > 0) {
            scale_dimension(&scaled_width, 1);
            scale_dimension(&scaled_height, 1);
            decimate(image, scaled_width, scaled_height);
            decimate(mask, scaled_width, scaled_height);
        } else {
            get_spatial_mask(image, mask, mask_dp, scaled_width, scaled_height);
        }

        filter_mode(image, scaled_width, scaled_height);

        calculate_c_values(image, mask, c_values, c_values_histograms, window_size,
                           tvi_for_diff, scaled_width, scaled_height);

        if (c_values_ret && c_values_ret[scale])
            memcpy(c_values_ret[scale], c_values, scaled_width * scaled_height * sizeof *c_values);

        scores_per_scale[scale] =
            spatial_pooling(c_values, topk, scaled_width, scaled_height);
    }

    uint16_t pixels_in_window = get_pixels_in_window(window_size);
    *score = weight_scores_per_scale(scores_per_scale, pixels_in_window);
    return 0;
}

int cambi_extract(CambiState *s, VmafPicture *pic, double *score, float **c_values) {
    int err = cambi_preprocessing(pic, &s->pics[0]);
    if (err) return err;

    err = cambi_score(s->pics, s->mask_dp, s->window_size, s->topk, s->tvi_for_diff, s->c_values, s->c_values_histograms, score, c_values);
    if (err) return err;

    return 0;
}

static int extract(VmafFeatureExtractor *fex,
                   VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90,
                   unsigned index, VmafFeatureCollector *feature_collector) {
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;

    CambiState *s = fex->priv;

    double score;
    int err = cambi_extract(s, dist_pic, &score, NULL);
    err = vmaf_feature_collector_append(feature_collector, "cambi", score, index);
    if (err) return err;

    return 0;
}

int cambi_close(CambiState *s) {
    int err = 0;
    for (unsigned i = 0; i < PICS_BUFFER_SIZE; i++)
        err |= vmaf_picture_unref(&s->pics[i]);

    aligned_free(s->c_values);
    aligned_free(s->c_values_histograms);
    aligned_free(s->mask_dp);
    return err;
}

static int close(VmafFeatureExtractor *fex)
{
    CambiState *s = fex->priv;
    return cambi_close(s);
}

static const char *provided_features[] = {
    "cambi",
    NULL
};

VmafFeatureExtractor vmaf_fex_cambi = {
    .name = "cambi",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close,
    .priv_size = sizeof(CambiState),
    .provided_features = provided_features,
};