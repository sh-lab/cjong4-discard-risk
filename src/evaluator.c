#include "cjong4_discard_risk/evaluator.h"

_Static_assert(sizeof(cj4dr_workspace) == 552, "workspace layout changed");

static uint8_t activate(int32_t sum, uint8_t shift, uint8_t maximum)
{
    if (sum <= 0)
        return 0;
    uint32_t value = (uint32_t)sum >> shift;
    return value > maximum ? maximum : (uint8_t)value;
}

bool cj4dr_evaluate(const cj4dr_model *model, const uint8_t *input, size_t size,
                   cj4dr_workspace *workspace, uint8_t *out_danger)
{
    if (!workspace || !out_danger || !cj4dr_model_validate(model) ||
        !cj4dr_validate_input(input, size))
        return false;

    for (unsigned image = 0; image < CJ4DR_IMAGE_COUNT; ++image) {
        cj4dr_image_layout layout;
        if (!cj4dr_get_image_layout((cj4dr_image)image, &layout))
            return false;
        unsigned bank = image < 3 ? 0 : image - 2;
        for (unsigned pixel = 0; pixel < layout.width * layout.height; ++pixel) {
            const uint8_t *rgb = input + layout.byte_offset + pixel * 3;
            size_t base = (layout.byte_offset / 3 + pixel) * CJ4DR_PIXEL_CHANNELS;
            for (unsigned channel = 0; channel < CJ4DR_PIXEL_CHANNELS; ++channel) {
                int32_t sum = model->pixel_bias[bank][channel];
                for (unsigned c = 0; c < 3; ++c)
                    sum += (int32_t)rgb[c] * model->pixel_weights[bank][channel][c];
                workspace->pixels[base + channel] =
                    activate(sum, model->pixel_shift[bank], 127);
            }
        }
    }
    cj4_tile_id candidate = input[CJ4DR_CANDIDATE_OFFSET];
    for (unsigned h = 0; h < CJ4DR_HIDDEN_SIZE; ++h) {
        int32_t sum = model->hidden_bias[h] + model->candidate_weights[candidate][h];
        for (unsigned i = 0; i < CJ4DR_CONTEXT_SIZE; ++i)
            sum += (int32_t)workspace->pixels[i] * model->hidden_weights[h][i];
        workspace->hidden[h] = activate(sum, model->hidden_shift, 127);
    }
    int32_t sum = model->output_bias;
    for (unsigned h = 0; h < CJ4DR_HIDDEN_SIZE; ++h)
        sum += (int32_t)workspace->hidden[h] * model->output_weights[h];
    *out_danger = activate(sum, model->output_shift, 255);
    return true;
}
