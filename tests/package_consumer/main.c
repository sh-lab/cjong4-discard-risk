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
    uint8_t danger = 0;
    memset(&model, 0, sizeof(model));
    model.version = CJ4DR_MODEL_VERSION;
    model.input_schema_version = CJ4DR_INPUT_SCHEMA_VERSION;
    model.output_bias = 42;
    return !(cj4dr_encode_input(&view, 0, input) &&
             cj4dr_validate_input(input, sizeof(input)) &&
             cj4dr_evaluate(&model, input, sizeof(input), &workspace, &danger) &&
             danger == 42);
}
