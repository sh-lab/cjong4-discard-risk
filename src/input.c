#include "cjong4_discard_risk/input.h"
#include <string.h>

_Static_assert(CJ4_TILE_ID_COUNT * 3 == CJ4DR_RGB_SIZE, "RGB layout changed");

static const cj4dr_image_layout layouts[CJ4DR_IMAGE_COUNT] = {
    {0,   9, 4, 0},
    {108, 9, 4, 9},
    {216, 9, 4, 18},
    {324, 4, 4, 27},
    {372, 3, 4, 31}
};

bool cj4dr_get_image_layout(cj4dr_image image, cj4dr_image_layout *out_layout)
{
    if ((unsigned)image >= CJ4DR_IMAGE_COUNT || !out_layout)
        return false;
    *out_layout = layouts[image];
    return true;
}

cj4_tile_id cj4dr_image_tile_id(cj4dr_image image, uint8_t x, uint8_t y)
{
    if ((unsigned)image >= CJ4DR_IMAGE_COUNT ||
        x >= layouts[image].width || y >= CJ4DR_IMAGE_HEIGHT)
        return CJ4_TILE_ID_INVALID;
    return (cj4_tile_id)((layouts[image].first_type + x) * 4 + y);
}

size_t cj4dr_tile_rgb_offset(cj4_tile_id tile)
{
    if (!cj4_tile_id_is_valid(tile))
        return SIZE_MAX;
    unsigned type = tile / 4;
    unsigned image = type < 27 ? type / 9 : (type < 31 ? 3 : 4);
    const cj4dr_image_layout *layout = &layouts[image];
    return layout->byte_offset +
           ((tile % 4) * layout->width + type - layout->first_type) * 3;
}

static bool location_is_valid(uint8_t discard, uint8_t placement,
                              uint8_t history, cj4_player observer)
{
    if (discard != CJ4_LOCATION_NONE && !cj4_location_is_discard(discard))
        return false;
    if (history != CJ4_LOCATION_NONE && !cj4_location_is_discard_history(history))
        return false;
    if (placement == CJ4_LOCATION_NONE)
        return true;
    if (cj4_location_is_hand(placement))
        return cj4_location_placement_player(placement) == observer;
    return cj4_location_is_meld(placement);
}

static uint8_t relative_owner(uint8_t value, cj4_player observer)
{
    if (value == CJ4_LOCATION_NONE)
        return value;
    unsigned owner = (value & CJ4_LOCATION_PLAYER_MASK) >> CJ4_LOCATION_PLAYER_SHIFT;
    unsigned relative = (owner + CJ4_PLAYER_COUNT - observer) % CJ4_PLAYER_COUNT;
    return (uint8_t)((value & ~CJ4_LOCATION_PLAYER_MASK) |
                     (relative << CJ4_LOCATION_PLAYER_SHIFT));
}

bool cj4dr_encode_input(const cj4_player_view *view, cj4_tile_id candidate,
                       uint8_t out_input[CJ4DR_INPUT_SIZE])
{
    uint8_t encoded[CJ4DR_INPUT_SIZE];
    if (!view || !out_input || view->player >= CJ4_PLAYER_COUNT ||
        !cj4_tile_id_is_valid(candidate))
        return false;
    for (unsigned tile = 0; tile < CJ4_TILE_ID_COUNT; ++tile) {
        const cj4_location *location = &view->locations[tile];
        if (!location_is_valid(location->discard, location->placement,
                               location->discard_history, view->player))
            return false;
        uint8_t *rgb = encoded + cj4dr_tile_rgb_offset((cj4_tile_id)tile);
        rgb[0] = relative_owner(location->discard, view->player);
        rgb[1] = relative_owner(location->placement, view->player);
        rgb[2] = location->discard_history;
    }
    encoded[CJ4DR_CANDIDATE_OFFSET] = candidate;
    memcpy(out_input, encoded, sizeof(encoded));
    return true;
}

bool cj4dr_validate_input(const uint8_t *input, size_t size)
{
    if (!input || size != CJ4DR_INPUT_SIZE)
        return false;
    for (size_t offset = 0; offset < CJ4DR_RGB_SIZE; offset += 3)
        if (!location_is_valid(input[offset], input[offset + 1], input[offset + 2], 0))
            return false;
    return cj4_tile_id_is_valid(input[CJ4DR_CANDIDATE_OFFSET]);
}

bool cj4dr_decode_input(const uint8_t *input, size_t size,
                       cj4_location out_locations[CJ4_TILE_ID_COUNT],
                       cj4_tile_id *out_candidate)
{
    if (!out_locations || !out_candidate || !cj4dr_validate_input(input, size))
        return false;
    for (unsigned tile = 0; tile < CJ4_TILE_ID_COUNT; ++tile) {
        const uint8_t *rgb = input + cj4dr_tile_rgb_offset((cj4_tile_id)tile);
        out_locations[tile].wall = CJ4_LOCATION_NONE;
        out_locations[tile].discard = rgb[0];
        out_locations[tile].placement = rgb[1];
        out_locations[tile].discard_history = rgb[2];
    }
    *out_candidate = input[CJ4DR_CANDIDATE_OFFSET];
    return true;
}
