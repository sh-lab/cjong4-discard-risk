#include "staged_teacher.h"
#include "cjong4/core/state_discard.h"
#include "cjong4/core/state_query.h"
#include <string.h>

void cj4dr_safety_observe(cj4dr_safety *safety, const cj4_mahjong *before,
                         const cj4_mahjong *after)
{
    cj4_phase phase = cj4_state_phase(after);
    if (after->discard_count == 0) {
        memset(safety, 0, sizeof(*safety));
        return;
    }
    /* Only a fully passed discard establishes new evidence. A competing call
     * does not prove that every other player declined an available ron. */
    if (cj4_state_phase(before) == CJ4_PHASE_DISCARD &&
        phase == CJ4_PHASE_DRAW) {
        cj4_tile_id tile = cj4_get_last_discard_tile(before);
        if (!cj4_location_is_meld(after->locations[tile].placement))
            for (cj4_player p = 0; p < CJ4_PLAYER_COUNT; ++p)
                if (p != cj4_state_current_player(before))
                    safety->passed[p][cj4_tile_get_type(tile)] = 1;
    }
    for (cj4_player p = 0; p < CJ4_PLAYER_COUNT; ++p) {
        bool meld_changed = false;
        for (unsigned id = 0; id < CJ4_TILE_ID_COUNT; ++id) {
            uint8_t a = before->locations[id].placement;
            uint8_t b = after->locations[id].placement;
            if (a != b &&
                ((cj4_location_is_meld(a) && cj4_location_placement_player(a) == p) ||
                 (cj4_location_is_meld(b) && cj4_location_placement_player(b) == p)))
                meld_changed = true;
        }
        bool drew_or_called = (phase == CJ4_PHASE_DRAW || phase == CJ4_PHASE_AFTER_CALL) &&
                              cj4_state_current_player(after) == p;
        /* At houtei a formerly yaku-less hand may gain a yaku. Do not claim
         * non-riichi passed tiles remain safe merely because no ron occurred. */
        if (meld_changed ||
            (!cj4_state_is_riichi(after, p) &&
             (drew_or_called || cj4_live_wall_remaining(after) == 0)))
            memset(safety->passed[p], 0, sizeof(safety->passed[p]));
    }
}

uint8_t cj4dr_stage_score(const cj4_player_view *view, const cj4dr_safety *safety,
                         cj4_tile_type type)
{
    static const uint8_t stage[3][4] = {
        {0, 16, 32, 64}, {0, 64, 96, 128}, {0, 128, 160, 192}
    };
    bool safe[CJ4_PLAYER_COUNT] = {false};
    cj4_discard_list discards = cj4_location_collect_discards(view->locations);
    for (unsigned i = 0; i < discards.count; ++i)
        if (cj4_tile_get_type(discards.items[i].tile) == type)
            safe[discards.items[i].player] = true; /* includes called-away discards */
    unsigned unknown = 0;
    uint8_t threat = 0;
    for (cj4_player p = 0; p < CJ4_PLAYER_COUNT; ++p) {
        if (p == view->player || safe[p] || safety->passed[p][type])
            continue;
        ++unknown;
        unsigned open = 0;
        cj4_meld_list melds = cj4_location_collect_melds(view->locations, p);
        for (unsigned m = 0; m < melds.count; ++m)
            open += melds.items[m].type != CJ4_MELD_ANKAN;
        uint8_t risk = view->is_riichi[p] || open >= 3 ? 192 : open ? 128 : 0;
        if (risk > threat) threat = risk;
    }
    unsigned period = view->live_wall_remaining >= 46 ? 0 :
                      view->live_wall_remaining >= 22 ? 1 : 2;
    uint8_t risk = stage[period][unknown];
    return risk > threat ? risk : threat;
}

bool cj4dr_make_staged_teacher(const cj4_mahjong *state, const cj4_rules *rules,
                              const cj4dr_safety *safety, cj4dr_teacher *output)
{
    if (!state || !rules || !safety || !output || !cj4_rules_validate(rules) ||
        (cj4_state_phase(state) != CJ4_PHASE_DRAW &&
         cj4_state_phase(state) != CJ4_PHASE_AFTER_CALL))
        return false;
    cj4dr_teacher result;
    memset(&result, 0, sizeof(result));
    cj4_player_view view = cj4m_make_player_view(state, cj4_state_current_player(state));
    if (!cj4dr_encode_input(&view, result.rgb))
        return false;
    unsigned labels = 0;
    for (unsigned id = 0; id < CJ4_TILE_ID_COUNT; ++id) {
        if (!cj4_can_discard(*state, rules, (cj4_tile_id)id))
            continue;
        cj4_tile_type type = cj4_tile_get_type((cj4_tile_id)id);
        if (result.mask[type]) continue;
        result.mask[type] = 1;
        result.target[type] = cj4dr_stage_score(&view, safety, type);
        ++labels;
    }
    if (!labels) return false;
    *output = result;
    return true;
}

bool cj4dr_mark_actual_ron(cj4dr_teacher *sample, int discarded)
{
    if (!sample || discarded < 0 || discarded >= CJ4_TILE_ID_COUNT ||
        !sample->mask[discarded / 4])
        return false;
    sample->target[discarded / 4] = 255;
    return true;
}
