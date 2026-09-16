#include <string.h>
#include "cjong4_discard_risk/evaluator.h"

int main(void)
{
    cj4_player_view view;
    uint8_t input[CJ4DR_INPUT_SIZE];
    memset(&view, 0, sizeof(view));
    memset(view.locations, CJ4_LOCATION_NONE, sizeof(view.locations));
    /* Synthetic constant model checks ABI only; not a risk model. */
    cj4dr_model model;
    cj4dr_workspace workspace;
    uint8_t danger[CJ4DR_OUTPUT_SIZE] = {0};
    memset(&model, 0, sizeof(model));
    model.version = CJ4DR_MODEL_VERSION;
    model.input_schema_version = CJ4DR_INPUT_SCHEMA_VERSION;
    for (unsigned t = 0; t < CJ4DR_OUTPUT_SIZE; ++t)
        model.output_bias[t] = (int32_t)t;
    if (!(cj4dr_encode_input(&view, input) &&
             cj4dr_validate_input(input, sizeof(input)) &&
             cj4dr_evaluate(&model, input, sizeof(input), &workspace, danger) &&
             danger[33] == 33))
        return 1;
    for (unsigned t = 0; t < CJ4DR_OUTPUT_SIZE; ++t)
        if (danger[t] != t)
            return 1;
    return 0;
}
