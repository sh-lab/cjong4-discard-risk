#include "cjong4_discard_risk/input.h"

#include <string.h>

_Static_assert(CJ4_TILE_ID_COUNT * CJ4DR_LOCATION_STRIDE == CJ4DR_DORA_OFFSET,
               "location layout changed");
_Static_assert(CJ4DR_DORA_OFFSET + CJ4DR_DORA_COUNT == CJ4DR_CANDIDATE_OFFSET,
               "indicator layout changed");
_Static_assert(CJ4DR_CANDIDATE_OFFSET + 1 == CJ4DR_INPUT_SIZE,
               "input layout changed");

static bool location_is_valid(uint8_t discard, uint8_t placement,
                              uint8_t history, cj4_player observer)
{
    if (discard != CJ4_LOCATION_NONE && !cj4_location_is_discard(discard))
        return false;
    if (history != CJ4_LOCATION_NONE &&
        !cj4_location_is_discard_history(history))
        return false;
    if (placement == CJ4_LOCATION_NONE)
        return true;
    if (cj4_location_is_hand(placement))
        return cj4_location_placement_player(placement) == observer;
    return cj4_location_is_meld(placement);
}

static uint8_t relative_owner(uint8_t value, cj4_player observer)
{
    unsigned owner = (value & CJ4_LOCATION_PLAYER_MASK) >>
                     CJ4_LOCATION_PLAYER_SHIFT;
    unsigned relative = (owner + CJ4_PLAYER_COUNT - observer) % CJ4_PLAYER_COUNT;
    return (uint8_t)((value & ~CJ4_LOCATION_PLAYER_MASK) |
                     (relative << CJ4_LOCATION_PLAYER_SHIFT));
}

static bool indicators_are_canonical(const uint8_t *indicators)
{
    for (size_t i = 0; i < CJ4DR_DORA_COUNT; ++i) {
        uint8_t tile = indicators[i];
        if (tile == CJ4_TILE_ID_INVALID)
            continue;
        if (!cj4_tile_id_is_valid(tile) || (i > 0 && tile <= indicators[i - 1]))
            return false;
    }
    return true;
}

bool cj4dr_encode_input(const cj4_player_view *view,
                       const cj4_tile_id dora_indicator_tile_ids[CJ4DR_DORA_COUNT],
                       cj4_tile_id candidate_tile_id,
                       uint8_t out_input[CJ4DR_INPUT_SIZE])
{
    uint8_t encoded[CJ4DR_INPUT_SIZE];
    if (!view || !dora_indicator_tile_ids || !out_input ||
        view->player >= CJ4_PLAYER_COUNT ||
        !cj4_tile_id_is_valid(candidate_tile_id))
        return false;

    for (size_t tile = 0; tile < CJ4_TILE_ID_COUNT; ++tile) {
        const cj4_location *location = &view->locations[tile];
        uint8_t *bytes = &encoded[tile * CJ4DR_LOCATION_STRIDE];
        if (!location_is_valid(location->discard, location->placement,
                               location->discard_history, view->player))
            return false;
        bytes[CJ4DR_DISCARD_OFFSET] = location->discard == CJ4_LOCATION_NONE
            ? CJ4_LOCATION_NONE : relative_owner(location->discard, view->player);
        bytes[CJ4DR_PLACEMENT_OFFSET] = location->placement == CJ4_LOCATION_NONE
            ? CJ4_LOCATION_NONE : relative_owner(location->placement, view->player);
        bytes[CJ4DR_DISCARD_HISTORY_OFFSET] = location->discard_history;
    }

    uint8_t *indicators = &encoded[CJ4DR_DORA_OFFSET];
    memcpy(indicators, dora_indicator_tile_ids, CJ4DR_DORA_COUNT);
    for (size_t i = 1; i < CJ4DR_DORA_COUNT; ++i) {
        uint8_t tile = indicators[i];
        size_t j = i;
        while (j > 0 && indicators[j - 1] > tile) {
            indicators[j] = indicators[j - 1];
            --j;
        }
        indicators[j] = tile;
    }
    if (!indicators_are_canonical(indicators))
        return false;
    encoded[CJ4DR_CANDIDATE_OFFSET] = candidate_tile_id;
    memcpy(out_input, encoded, sizeof(encoded));
    return true;
}

bool cj4dr_validate_input(const uint8_t *input, size_t size)
{
    if (!input || size != CJ4DR_INPUT_SIZE)
        return false;
    for (size_t tile = 0; tile < CJ4_TILE_ID_COUNT; ++tile) {
        const uint8_t *bytes = &input[tile * CJ4DR_LOCATION_STRIDE];
        if (!location_is_valid(bytes[CJ4DR_DISCARD_OFFSET],
                               bytes[CJ4DR_PLACEMENT_OFFSET],
                               bytes[CJ4DR_DISCARD_HISTORY_OFFSET], 0))
            return false;
    }
    return indicators_are_canonical(&input[CJ4DR_DORA_OFFSET]) &&
           cj4_tile_id_is_valid(input[CJ4DR_CANDIDATE_OFFSET]);
}
