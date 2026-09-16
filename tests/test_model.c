#include <assert.h>
#include <limits.h>
#include <string.h>
#include "cjong4_discard_risk/evaluator.h"

_Static_assert(CJ4DR_OUTPUT_SIZE == 34, "output must be 34x1");
_Static_assert(CJ4DR_MODEL_VERSION == 2, "update model fixtures");
_Static_assert(CJ4DR_MODEL_SERIALIZED_SIZE == 4897, "update file offsets");

static cj4dr_model empty_model(void)
{
    cj4dr_model model;
    memset(&model, 0, sizeof(model));
    model.version = CJ4DR_MODEL_VERSION;
    model.input_schema_version = CJ4DR_INPUT_SCHEMA_VERSION;
    return model;
}

/* INT64 and division-based oracle, independent of the INT32/shift kernel. */
static uint8_t reference_activate(int64_t n, unsigned shift, unsigned cap)
{
    if (n < 0)
        n = 0;
    n /= (int64_t)1 << shift;
    if (n > cap)
        n = cap;
    return (uint8_t)n;
}

static void reference(const cj4dr_model *model, const uint8_t *input, uint8_t out[34])
{
    uint8_t pixels[544], hidden[8];
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
    for (unsigned h = 0; h < 8; ++h) {
        int64_t n = model->hidden_bias[h];
        for (unsigned i = 0; i < 544; ++i)
            n += (int64_t)pixels[i] * model->hidden_weights[h][i];
        hidden[h] = reference_activate(n, model->hidden_shift, 127);
    }
    for (unsigned t = 0; t < 34; ++t) {
        int64_t n = model->output_bias[t];
        for (unsigned h = 0; h < 8; ++h)
            n += (int64_t)hidden[h] * model->output_weights[t][h];
        out[t] = reference_activate(n, model->output_shift, 255);
    }
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
    }
    for (unsigned t = 0; t < 34; ++t) {
        for (unsigned h = 0; h < 8; ++h)
            model->output_weights[t][h] = random_weight(rng);
        model->output_bias[t] = (int32_t)(random_word(rng) % 32768) - 8192;
    }
    model->hidden_shift = 10;
    model->output_shift = 6;
}

static void test_known_inference(void)
{
    cj4dr_model model = empty_model();
    cj4dr_workspace workspace;
    uint8_t input[408], guarded[36];
    memset(input, 255, sizeof(input));
    input[0] = 10;
    model.pixel_weights[0][0][0] = 1;
    model.pixel_shift[0] = 1;
    model.hidden_weights[0][0] = 2;
    model.output_shift = 1;
    for (unsigned t = 0; t < 34; ++t) {
        model.output_weights[t][0] = (int8_t)((int)(t % 5) - 2);
        model.output_bias[t] = (int32_t)t * 19 - 100;
    }
    memset(guarded, 0xa5, sizeof(guarded));
    assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, guarded + 1));
    assert(guarded[0] == 0xa5 && guarded[35] == 0xa5);
    for (unsigned t = 0; t < 34; ++t) {
        /* hidden[0] = (10/2)*2 = 10; distinct output rows, clipped at both ends. */
        int expected = (10 * ((int)(t % 5) - 2) + (int)t * 19 - 100);
        expected = expected < 0 ? 0 : expected / 2;
        expected = expected > 255 ? 255 : expected;
        assert(guarded[t + 1] == expected);
    }

    /* Every type, including absent and unavailable tiles, must receive its score. */
    cj4_player_view view;
    memset(&view, 0, sizeof(view));
    memset(view.locations, 255, sizeof(view.locations));
    view.locations[0].placement = 0;
    assert(cj4dr_encode_input(&view, input));
    model = empty_model();
    for (unsigned t = 0; t < 34; ++t)
        model.output_bias[t] = (int32_t)t + 1;
    assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, guarded + 1));
    for (unsigned t = 0; t < 34; ++t)
        assert(guarded[t + 1] == t + 1);

    /* All five regions feed the shared hidden state and all 34 output heads. */
    const unsigned starts[] = {0, 108, 216, 324, 372};
    for (unsigned region = 0; region < 5; ++region) {
        model = empty_model();
        memset(input, 255, sizeof(input));
        input[starts[region]] = 7;
        for (unsigned bank = 0; bank < 3; ++bank)
            model.pixel_weights[bank][0][0] = (int8_t)(bank + 1);
        model.hidden_weights[0][starts[region] / 3 * 4] = 1;
        for (unsigned t = 0; t < 34; ++t) {
            model.output_weights[t][0] = 1;
            model.output_bias[t] = (int32_t)t;
        }
        assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, guarded + 1));
        for (unsigned t = 0; t < 34; ++t)
            assert(guarded[t + 1] == 7 * (region < 3 ? 1 : region - 1) + t);
    }
}

static void test_reference_arithmetic(void)
{
    uint32_t rng = 20260916;
    cj4dr_model model;
    cj4dr_workspace workspace;
    uint8_t input[408], danger[34], expected[34];
    for (unsigned trial = 0; trial < 256; ++trial) {
        fill_model(&model, &rng);
        for (unsigned i = 0; i < 136; ++i) {
            input[i * 3] = (uint8_t)(random_word(&rng) % 31 + (i % 4) * 32);
            input[i * 3 + 1] = 255;
            input[i * 3 + 2] = (uint8_t)(random_word(&rng) % 86 + (i % 2) * 128);
        }
        assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, danger));
        reference(&model, input, expected);
        assert(memcmp(danger, expected, sizeof(danger)) == 0);
    }
    memset(input, 255, sizeof(input));
    for (unsigned negative = 0; negative < 2; ++negative) {
        model = empty_model();
        memset(model.pixel_weights, 127, sizeof(model.pixel_weights));
        memset(model.hidden_weights, negative ? 128 : 127, sizeof(model.hidden_weights));
        memset(model.output_weights, negative ? 128 : 127, sizeof(model.output_weights));
        for (unsigned b = 0; b < 3; ++b)
            for (unsigned c = 0; c < 4; ++c)
                model.pixel_bias[b][c] = CJ4DR_MODEL_BIAS_LIMIT;
        for (unsigned h = 0; h < 8; ++h)
            model.hidden_bias[h] = negative ? -CJ4DR_MODEL_BIAS_LIMIT : CJ4DR_MODEL_BIAS_LIMIT;
        for (unsigned t = 0; t < 34; ++t)
            model.output_bias[t] = negative ? -CJ4DR_MODEL_BIAS_LIMIT : CJ4DR_MODEL_BIAS_LIMIT;
        for (unsigned shift = 0; shift <= 31; ++shift) {
            model.pixel_shift[0] = model.pixel_shift[1] = model.pixel_shift[2] = (uint8_t)shift;
            model.hidden_shift = model.output_shift = (uint8_t)shift;
            assert(cj4dr_evaluate(&model, input, sizeof(input), &workspace, danger));
            reference(&model, input, expected);
            assert(memcmp(danger, expected, sizeof(danger)) == 0);
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
    model.output_weights[33][7] = -127;
    model.output_bias[33] = -3;
    memset(buffer, 0xa5, sizeof(buffer));
    assert(cj4dr_model_encode(&model, buffer + 1, sizeof(buffer) - 2));
    assert(buffer[0] == 0xa5 && buffer[sizeof(buffer) - 1] == 0xa5);
    uint8_t *bytes = buffer + 1;
    assert(memcmp(bytes, "CJ4DRI8", 8) == 0);
    assert(bytes[8] == 2 && bytes[10] == 3);
    assert(bytes[12] == 0x21 && bytes[13] == 0x13); /* 4897 */
    assert(bytes[16] == 128);
    assert(bytes[52] == 254 && bytes[53] == 255 && bytes[54] == 255 && bytes[55] == 255);
    assert(bytes[4759] == 129 && bytes[4892] == 253);
    assert(cj4dr_model_decode(bytes, 4897, &decoded));
    assert(cj4dr_model_encode(&decoded, repeated, sizeof(repeated)));
    assert(memcmp(bytes, repeated, sizeof(repeated)) == 0);
    assert(decoded.output_weights[33][7] == -127 && decoded.output_bias[33] == -3);

    const size_t corrupt_offsets[] = {0, 7, 8, 9, 10, 11, 12, 15, 100, 101, 102,
                                     55, 4486, 4487, 4763, 4895, 4896};
    for (unsigned i = 0; i < sizeof(corrupt_offsets) / sizeof(corrupt_offsets[0]); ++i) {
        memcpy(bytes, repeated, sizeof(repeated));
        bytes[corrupt_offsets[i]] = 127;
        memset(&decoded, 0xa5, sizeof(decoded));
        assert(!cj4dr_model_decode(bytes, 4897, &decoded));
        const unsigned char *raw = (const unsigned char *)&decoded;
        for (size_t j = 0; j < sizeof(decoded); ++j)
            assert(raw[j] == 0xa5);
    }
    /* Explicit old model/schema versions, even when size is forged to v2. */
    memcpy(bytes, repeated, sizeof(repeated));
    bytes[8] = 1;
    assert(!cj4dr_model_decode(bytes, 4897, &decoded));
    bytes[8] = 2;
    bytes[10] = 2;
    assert(!cj4dr_model_decode(bytes, 4897, &decoded));
    assert(!cj4dr_model_decode(NULL, 4897, &decoded));
    assert(!cj4dr_model_decode(repeated, 4897, NULL));
    assert(!cj4dr_model_decode(repeated, 0, &decoded));
    assert(!cj4dr_model_decode(repeated, 4896, &decoded));
    assert(!cj4dr_model_decode(repeated, 4898, &decoded));
    assert(!cj4dr_model_decode(repeated, 5589, &decoded));
    model.output_bias[33] = INT32_MAX;
    memset(buffer, 0xa5, sizeof(buffer));
    assert(!cj4dr_model_encode(&model, buffer, 4897));
    for (unsigned i = 0; i < sizeof(buffer); ++i)
        assert(buffer[i] == 0xa5);
    assert(!cj4dr_model_encode(NULL, buffer, 4897));
    assert(!cj4dr_model_encode(&model, NULL, 4897));
    assert(!cj4dr_model_encode(&model, buffer, 4896));
}

static void test_inference_errors(void)
{
    cj4dr_model model = empty_model();
    cj4dr_workspace workspace;
    uint8_t input[408], danger[34];
    memset(input, 255, sizeof(input));
    memset(danger, 0xa5, sizeof(danger));
    memset(&workspace, 0xa5, sizeof(workspace));
    assert(!cj4dr_evaluate(NULL, input, 408, &workspace, danger));
    assert(!cj4dr_evaluate(&model, NULL, 408, &workspace, danger));
    assert(!cj4dr_evaluate(&model, input, 407, &workspace, danger));
    assert(!cj4dr_evaluate(&model, input, 409, &workspace, danger));
    assert(!cj4dr_evaluate(&model, input, 414, &workspace, danger));
    assert(!cj4dr_evaluate(&model, input, 408, NULL, danger));
    assert(!cj4dr_evaluate(&model, input, 408, &workspace, NULL));
    input[407] = 127;
    assert(!cj4dr_evaluate(&model, input, 408, &workspace, danger));
    input[407] = 255;
    model.version = 1;
    assert(!cj4dr_evaluate(&model, input, 408, &workspace, danger));
    model.version = 2;
    model.input_schema_version = 2;
    assert(!cj4dr_evaluate(&model, input, 408, &workspace, danger));
    model.input_schema_version = 3;
    model.pixel_shift[1] = 32;
    assert(!cj4dr_evaluate(&model, input, 408, &workspace, danger));
    model.pixel_shift[1] = 0;
    model.hidden_bias[7] = INT32_MIN;
    assert(!cj4dr_evaluate(&model, input, 408, &workspace, danger));
    model.hidden_bias[7] = 0;
    for (unsigned t = 0; t < 34; ++t) {
        model.output_bias[t] = INT32_MAX;
        assert(!cj4dr_evaluate(&model, input, 408, &workspace, danger));
        model.output_bias[t] = 0;
    }
    for (unsigned t = 0; t < 34; ++t)
        assert(danger[t] == 0xa5);
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
