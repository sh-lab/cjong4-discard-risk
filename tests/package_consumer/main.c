#include <string.h>
#include "cjong4_discard_risk/input.h"

int main(void)
{
    cj4_player_view view;
    const cj4_tile_id indicators[CJ4DR_DORA_COUNT] = {135, 255, 255, 255, 255};
    uint8_t input[CJ4DR_INPUT_SIZE];
    memset(&view, 0, sizeof(view));
    memset(view.locations, CJ4_LOCATION_NONE, sizeof(view.locations));
    return !(cj4dr_encode_input(&view, indicators, 0, input) &&
             cj4dr_validate_input(input, sizeof(input)));
}
