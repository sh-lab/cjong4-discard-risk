#include <assert.h>
#include <limits.h>
#include <string.h>
#include "cjong4_discard_risk/evaluator.h"

static cj4dr_model empty_model(void)
{
    cj4dr_model model;
    memset(&model, 0, sizeof(model));
    model.version = CJ4DR_MODEL_VERSION;
    model.input_schema_version = CJ4DR_INPUT_SCHEMA_VERSION;
    return model;
}

/* Deliberately separate INT64, division-based implementation to check the
 * INT32 inference arithmetic, candidate selection, ordering, and saturation. */
static uint8_t reference_activate(int64_t n, unsigned shift, unsigned cap)
{
    if (n < 0)
        n = 0;
    n /= (int64_t)1 << shift;
    if (n > cap)
        n = cap;
    return (uint8_t)n;
}

static uint8_t reference(const cj4dr_model *model, const uint8_t *input)
{
    uint8_t pixels[544];
    for (unsigned i = 0; i < 136; ++i) {
        unsigned bank = i < 108 ? 0 : (i < 124 ? 1 : 2);
        for (unsigned c = 0; c < 4; ++c) {
            int64_t n = model->pixel_bias[bank][c];
            n += (int64_t)input[i * 3] * model->pixel_weights[bank][c][0];
            n += (int64_t)input[i * 3 + 1] * model->pixel_weights[bank][c][1];
            n += (int64_t)input[i * 3 + 2] * model->pixel_weights[bank][c][2];
            pixels[i * 4 + c] = reference_activate(n, model->pixel_shift[bank], 127);
        }
    }
    int64_t score = model->output_bias;
    for (unsigned h = 0; h < 8; ++h) {
        int64_t n = model->hidden_bias[h];
        n += model->candidate_weights[input[408]][h];
        for (unsigned i = 0; i < 544; ++i)
            n += (int64_t)pixels[i] * model->hidden_weights[h][i];
        score += (int64_t)reference_activate(n, model->hidden_shift, 127) *
                 model->output_weights[h];
    }
    return reference_activate(score, model->output_shift, 255);
}

static uint32_t random_word(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int8_t random_weight(uint32_t *state)
{
    return (int8_t)((int)(random_word(state) >> 24) - 128);
}

static void fill_model(cj4dr_model *model, uint32_t *rng)
{
    *model = empty_model();
    for (unsigned b = 0; b < 3; ++b) {
        model->pixel_shift[b] = 8;
        for (unsigned c = 0; c < 4; ++c) {
            model->pixel_bias[b][c] = (int32_t)(random_word(rng) % 65536) - 32768;
            for (unsigned k = 0; k < 3; ++k)
                model->pixel_weights[b][c][k] = random_weight(rng);
        }
    }
    for (unsigned h = 0; h < 8; ++h) {
        model->hidden_bias[h] = (int32_t)(random_word(rng) % 65536) - 32768;
        for (unsigned i = 0; i < 544; ++i)
            model->hidden_weights[h][i] = random_weight(rng);
        model->output_weights[h] = random_weight(rng);
    }
    for (unsigned tile = 0; tile < 136; ++tile)
        for (unsigned h = 0; h < 8; ++h)
            model->candidate_weights[tile][h] = random_weight(rng);
    model->hidden_shift = 10;
    model->output_shift = 6;
    model->output_bias = 8192;
}

static void test_known_inference(void)
{
    cj4dr_model model = empty_model();
    cj4dr_workspace workspace;
    uint8_t input[409], danger;
    memset(input, 255, sizeof(input));
    input[0] = 10;
    input[408] = 13;
    model.pixel_weights[0][0][0] = 1;
    model.pixel_shift[0] = 1;
    model.hidden_weights[0][0] = 2;
    model.candidate_weights[13][0] = 3;
    model.candidate_weights[14][0] = 5;
    model.output_weights[0] = 4;
    model.output_bias = 1;
    model.output_shift = 1;
    assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, &danger));
    assert(danger == 26); /* floor(((10/2)*2 + 3)*4 + 1)/2 */
    input[408] = 14; /* Same RGB image, different physical candidate. */
    assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, &danger));
    assert(danger == 30);

    /* Prove all five image regions contribute, with suit weights shared. */
    const unsigned starts[] = {0, 108, 216, 324, 372};
    for (unsigned region = 0; region < 5; ++region) {
        model = empty_model();
        memset(input, 255, sizeof(input));
        input[408] = 0;
        input[starts[region]] = 7;
        for (unsigned bank = 0; bank < 3; ++bank)
            model.pixel_weights[bank][0][0] = (int8_t)(bank + 1);
        model.hidden_weights[0][starts[region] / 3 * 4] = 1;
        model.output_weights[0] = 1;
        assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, &danger));
        assert(danger == 7 * (region < 3 ? 1 : region - 1));
    }
}

static void test_reference_arithmetic(void)
{
    uint32_t rng = 20260915;
    cj4dr_model model;
    cj4dr_workspace workspace;
    uint8_t input[409], danger;
    for (unsigned trial = 0; trial < 256; ++trial) {
        fill_model(&model, &rng);
        for (unsigned i = 0; i < 136; ++i) {
            input[i * 3] = (uint8_t)(random_word(&rng) % 31 + (i % 4) * 32);
            input[i * 3 + 1] = 255;
            input[i * 3 + 2] = (uint8_t)(random_word(&rng) % 86 + (i % 2) * 128);
        }
        input[408] = (uint8_t)(trial % 136);
        assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, &danger));
        assert(danger == reference(&model, input));
    }
    /* Valid worst-case biases/weights, all supported shifts, negative sums. */
    memset(input, 255, sizeof(input));
    input[408] = 135;
    for (unsigned negative = 0; negative < 2; ++negative) {
        model = empty_model();
        memset(model.pixel_weights, 127, sizeof(model.pixel_weights));
        memset(model.hidden_weights, negative ? 128 : 127, sizeof(model.hidden_weights));
        memset(model.candidate_weights, negative ? 128 : 127, sizeof(model.candidate_weights));
        memset(model.output_weights, negative ? 128 : 127, sizeof(model.output_weights));
        for (unsigned b = 0; b < 3; ++b)
            for (unsigned c = 0; c < 4; ++c)
                model.pixel_bias[b][c] = CJ4DR_MODEL_BIAS_LIMIT;
        for (unsigned h = 0; h < 8; ++h)
            model.hidden_bias[h] = negative ? -CJ4DR_MODEL_BIAS_LIMIT : CJ4DR_MODEL_BIAS_LIMIT;
        model.output_bias = negative ? -CJ4DR_MODEL_BIAS_LIMIT : CJ4DR_MODEL_BIAS_LIMIT;
        for (unsigned shift = 0; shift <= 31; ++shift) {
            model.pixel_shift[0] = model.pixel_shift[1] = model.pixel_shift[2] = (uint8_t)shift;
            model.hidden_shift = model.output_shift = (uint8_t)shift;
            assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, &danger));
            assert(danger == reference(&model, input));
        }
    }
}

static void test_model_io(void)
{
    uint32_t rng = 42;
    cj4dr_model model, decoded;
    uint8_t buffer[CJ4DR_MODEL_SERIALIZED_SIZE + 2], repeated[CJ4DR_MODEL_SERIALIZED_SIZE];
    fill_model(&model, &rng);
    model.pixel_weights[0][0][0] = -128;
    model.pixel_bias[0][0] = -2;
    memset(buffer, 0xa5, sizeof(buffer));
    assert(cj4dr_model_encode(&model, buffer + 1, sizeof(buffer) - 2));
    assert(buffer[0] == 0xa5 && buffer[sizeof(buffer) - 1] == 0xa5);
    uint8_t *bytes = buffer + 1;
    assert(memcmp(bytes, "CJ4DRI8", 8) == 0);
    assert(bytes[8] == 1 && bytes[10] == 2);
    assert(bytes[12] == 0xd5 && bytes[13] == 0x15); /* 5589 */
    assert(bytes[16] == 128);
    assert(bytes[52] == 254 && bytes[53] == 255 && bytes[54] == 255 && bytes[55] == 255);
    assert(cj4dr_model_decode(bytes, 5589, &decoded));
    assert(cj4dr_model_encode(&decoded, repeated, sizeof(repeated)));
    assert(memcmp(bytes, repeated, sizeof(repeated)) == 0);
    assert(decoded.pixel_weights[0][0][0] == -128 && decoded.pixel_bias[0][0] == -2);

    /* Header, shift and bias failures must not partially replace a model. */
    const size_t corrupt_offsets[] = {0, 7, 8, 9, 10, 11, 12, 15, 100, 101, 102,
                                     55, 5574, 5575, 5587, 5588};
    for (unsigned i = 0; i < sizeof(corrupt_offsets) / sizeof(corrupt_offsets[0]); ++i) {
        memcpy(bytes, repeated, sizeof(repeated));
        bytes[corrupt_offsets[i]] = 127;
        memset(&decoded, 0xa5, sizeof(decoded));
        assert(!cj4dr_model_decode(bytes, 5589, &decoded));
        const unsigned char *raw = (const unsigned char *)&decoded;
        for (size_t j = 0; j < sizeof(decoded); ++j)
            assert(raw[j] == 0xa5);
    }
    assert(!cj4dr_model_decode(NULL, 5589, &decoded));
    assert(!cj4dr_model_decode(repeated, 5589, NULL));
    assert(!cj4dr_model_decode(repeated, 0, &decoded));
    assert(!cj4dr_model_decode(repeated, 5588, &decoded));
    assert(!cj4dr_model_decode(repeated, 5590, &decoded));
    model.output_bias = INT32_MAX;
    memset(buffer, 0xa5, sizeof(buffer));
    assert(!cj4dr_model_encode(&model, buffer, 5589));
    for (unsigned i = 0; i < sizeof(buffer); ++i)
        assert(buffer[i] == 0xa5);
    assert(!cj4dr_model_encode(NULL, buffer, 5589));
    assert(!cj4dr_model_encode(&model, NULL, 5589));
    assert(!cj4dr_model_encode(&model, buffer, 5588));
}

static void test_inference_errors(void)
{
    cj4dr_model model = empty_model();
    cj4dr_workspace workspace;
    uint8_t input[409], danger = 42;
    memset(input, 255, sizeof(input));
    input[408] = 0;
    memset(&workspace, 0xa5, sizeof(workspace));
    assert(!cj4dr_evaluate(NULL, input, 409, &workspace, &danger));
    assert(!cj4dr_evaluate(&model, NULL, 409, &workspace, &danger));
    assert(!cj4dr_evaluate(&model, input, 408, &workspace, &danger));
    assert(!cj4dr_evaluate(&model, input, 414, &workspace, &danger));
    assert(!cj4dr_evaluate(&model, input, 409, NULL, &danger));
    assert(!cj4dr_evaluate(&model, input, 409, &workspace, NULL));
    input[408] = 255;
    assert(!cj4dr_evaluate(&model, input, 409, &workspace, &danger));
    input[408] = 0;
    model.version = 0;
    assert(!cj4dr_evaluate(&model, input, 409, &workspace, &danger));
    model.version = 1;
    model.input_schema_version = 1;
    assert(!cj4dr_evaluate(&model, input, 409, &workspace, &danger));
    model.input_schema_version = 2;
    model.pixel_shift[1] = 32;
    assert(!cj4dr_evaluate(&model, input, 409, &workspace, &danger));
    model.pixel_shift[1] = 0;
    model.hidden_bias[7] = INT32_MIN;
    assert(!cj4dr_evaluate(&model, input, 409, &workspace, &danger));
    assert(danger == 42);
    const unsigned char *raw = (const unsigned char *)&workspace;
    for (unsigned i = 0; i < sizeof(workspace); ++i)
        assert(raw[i] == 0xa5);
}

void test_model(void)
{
    test_known_inference();
    test_reference_arithmetic();
    test_model_io();
    test_inference_errors();
}
