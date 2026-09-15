#include "cjong4_discard_risk/model.h"
#include <limits.h>
#include <string.h>

_Static_assert(CJ4DR_WEIGHT_COUNT + CJ4DR_BIAS_COUNT * 4 + 5 + 16 ==
               CJ4DR_MODEL_SERIALIZED_SIZE, "model layout changed");

static const uint8_t magic[8] = {'C', 'J', '4', 'D', 'R', 'I', '8', 0};

static bool bias_valid(int32_t bias)
{
    return bias >= -CJ4DR_MODEL_BIAS_LIMIT && bias <= CJ4DR_MODEL_BIAS_LIMIT;
}

bool cj4dr_model_validate(const cj4dr_model *model)
{
    if (!model || model->version != CJ4DR_MODEL_VERSION ||
        model->input_schema_version != CJ4DR_INPUT_SCHEMA_VERSION ||
        model->hidden_shift > CJ4DR_MODEL_SHIFT_MAX ||
        model->output_shift > CJ4DR_MODEL_SHIFT_MAX || !bias_valid(model->output_bias))
        return false;
    for (unsigned bank = 0; bank < CJ4DR_PIXEL_BANKS; ++bank) {
        if (model->pixel_shift[bank] > CJ4DR_MODEL_SHIFT_MAX)
            return false;
        for (unsigned c = 0; c < CJ4DR_PIXEL_CHANNELS; ++c)
            if (!bias_valid(model->pixel_bias[bank][c]))
                return false;
    }
    for (unsigned h = 0; h < CJ4DR_HIDDEN_SIZE; ++h)
        if (!bias_valid(model->hidden_bias[h]))
            return false;
    return true;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

static int32_t get_i32(const uint8_t *p)
{
    uint32_t value = get_u32(p);
    return value <= INT32_MAX ? (int32_t)value : -1 - (int32_t)(UINT32_MAX - value);
}

static int8_t get_i8(uint8_t value)
{
    return (int8_t)(value < 128 ? (int)value : (int)value - 256);
}

/* Same field walk for encoding and decoding; no struct padding or native
 * signed/endian representation is written. Exactly 5573 payload bytes. */
#define MODEL_FIELDS(I8, I32, U8) \
    for (unsigned b = 0; b < CJ4DR_PIXEL_BANKS; ++b) \
        for (unsigned c = 0; c < CJ4DR_PIXEL_CHANNELS; ++c) \
            for (unsigned k = 0; k < 3; ++k) { I8(pixel_weights[b][c][k]); } \
    for (unsigned b = 0; b < CJ4DR_PIXEL_BANKS; ++b) \
        for (unsigned c = 0; c < CJ4DR_PIXEL_CHANNELS; ++c) { I32(pixel_bias[b][c]); } \
    for (unsigned b = 0; b < CJ4DR_PIXEL_BANKS; ++b) { U8(pixel_shift[b]); } \
    for (unsigned h = 0; h < CJ4DR_HIDDEN_SIZE; ++h) \
        for (unsigned i = 0; i < CJ4DR_CONTEXT_SIZE; ++i) { I8(hidden_weights[h][i]); } \
    for (unsigned t = 0; t < CJ4_TILE_ID_COUNT; ++t) \
        for (unsigned h = 0; h < CJ4DR_HIDDEN_SIZE; ++h) { I8(candidate_weights[t][h]); } \
    for (unsigned h = 0; h < CJ4DR_HIDDEN_SIZE; ++h) { I32(hidden_bias[h]); } \
    U8(hidden_shift); \
    for (unsigned h = 0; h < CJ4DR_HIDDEN_SIZE; ++h) { I8(output_weights[h]); } \
    I32(output_bias); \
    U8(output_shift)

bool cj4dr_model_encode(const cj4dr_model *model, uint8_t *output, size_t size)
{
    if (!output || size != CJ4DR_MODEL_SERIALIZED_SIZE || !cj4dr_model_validate(model))
        return false;
    memcpy(output, magic, 8);
    output[8] = CJ4DR_MODEL_VERSION;
    output[9] = 0;
    output[10] = CJ4DR_INPUT_SCHEMA_VERSION;
    output[11] = 0;
    put_u32(output + 12, CJ4DR_MODEL_SERIALIZED_SIZE);
    uint8_t *cursor = output + 16;
#define WRITE_I8(field) *cursor++ = (uint8_t)model->field
#define WRITE_I32(field) put_u32(cursor, (uint32_t)model->field); cursor += 4
#define WRITE_U8(field) *cursor++ = model->field
    MODEL_FIELDS(WRITE_I8, WRITE_I32, WRITE_U8);
#undef WRITE_I8
#undef WRITE_I32
#undef WRITE_U8
    return true;
}

bool cj4dr_model_decode(const uint8_t *data, size_t size, cj4dr_model *out_model)
{
    if (!data || !out_model || size != CJ4DR_MODEL_SERIALIZED_SIZE)
        return false;
    if (memcmp(data, magic, 8) != 0 || data[8] != CJ4DR_MODEL_VERSION || data[9] != 0 ||
        data[10] != CJ4DR_INPUT_SCHEMA_VERSION || data[11] != 0 ||
        get_u32(data + 12) != CJ4DR_MODEL_SERIALIZED_SIZE)
        return false;
    cj4dr_model decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.version = CJ4DR_MODEL_VERSION;
    decoded.input_schema_version = CJ4DR_INPUT_SCHEMA_VERSION;
    const uint8_t *cursor = data + 16;
#define READ_I8(field) decoded.field = get_i8(*cursor++)
#define READ_I32(field) decoded.field = get_i32(cursor); cursor += 4
#define READ_U8(field) decoded.field = *cursor++
    MODEL_FIELDS(READ_I8, READ_I32, READ_U8);
#undef READ_I8
#undef READ_I32
#undef READ_U8
    if (!cj4dr_model_validate(&decoded))
        return false;
    *out_model = decoded;
    return true;
}
