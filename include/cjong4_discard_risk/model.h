#ifndef CJ4DR_MODEL_H
#define CJ4DR_MODEL_H

#include "cjong4_discard_risk/input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CJ4DR_MODEL_VERSION 2u
enum {
    /* One bank shared by all suits, one for winds, one for dragons. */
    CJ4DR_PIXEL_BANKS = 3,
    CJ4DR_PIXEL_CHANNELS = 4,
    CJ4DR_CONTEXT_SIZE = 136 * CJ4DR_PIXEL_CHANNELS,
    CJ4DR_HIDDEN_SIZE = 8,
    /* 34x1 grayscale, indexed by cj4_tile_type (not physical tile ID). */
    CJ4DR_OUTPUT_SIZE = CJ4_TILE_TYPE_COUNT,
    CJ4DR_WEIGHT_COUNT = 4660,
    CJ4DR_BIAS_COUNT = 54,
    CJ4DR_MODEL_SERIALIZED_SIZE = 4897,
    CJ4DR_MODEL_BIAS_LIMIT = 16777216,
    CJ4DR_MODEL_SHIFT_MAX = 31
};

/* Caller-owned INT8 weights, INT32 biases. Not a file format.
 * Hidden rows use image order, then row-major pixel order, then channel order.
 * Output rows are independent tile-type scores in cjong4 order 0..33.
 * No candidate input, legality filter, teacher mask, or safety-rule override.
 * No trained/default weights are bundled. Validation certifies format and
 * arithmetic bounds, not prediction quality or calibration. */
typedef struct {
    uint32_t version;
    uint32_t input_schema_version;
    int8_t pixel_weights[CJ4DR_PIXEL_BANKS][CJ4DR_PIXEL_CHANNELS][3];
    int32_t pixel_bias[CJ4DR_PIXEL_BANKS][CJ4DR_PIXEL_CHANNELS];
    uint8_t pixel_shift[CJ4DR_PIXEL_BANKS];
    int8_t hidden_weights[CJ4DR_HIDDEN_SIZE][CJ4DR_CONTEXT_SIZE];
    int32_t hidden_bias[CJ4DR_HIDDEN_SIZE];
    uint8_t hidden_shift;
    int8_t output_weights[CJ4DR_OUTPUT_SIZE][CJ4DR_HIDDEN_SIZE];
    int32_t output_bias[CJ4DR_OUTPUT_SIZE];
    uint8_t output_shift;
} cj4dr_model;

bool cj4dr_model_validate(const cj4dr_model *model);

/* Portable little-endian v2 format, exactly CJ4DR_MODEL_SERIALIZED_SIZE bytes.
 * Requires input schema v3. v1 single-candidate models are incompatible.
 * No file I/O or allocation. NULL/wrong lengths are rejected before reading.
 * Sources and outputs must not overlap. Failure leaves output unchanged.
 * The decoder uses one temporary model to preserve output on failure. */
bool cj4dr_model_encode(const cj4dr_model *model, uint8_t *output, size_t size);
bool cj4dr_model_decode(const uint8_t *data, size_t size, cj4dr_model *out_model);

#ifdef __cplusplus
}
#endif
#endif /* CJ4DR_MODEL_H */
