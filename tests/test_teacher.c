#include "teacher.h"
#include "cjong4/core/state_discard.h"
#include "cjong4/core/state_pass.h"
#include "cjong4/core/state_query.h"
#include "cjong4/core/state_ron.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_sample(const cj4_mahjong *state, const cj4_rules *rules)
{
    cj4_mahjong original = *state;
    cj4dr_teacher teacher;
    assert(cj4dr_make_teacher(state, rules, &teacher));
    assert(memcmp(state, &original, sizeof(original)) == 0);
    uint8_t rgb[408], legal[34] = {0};
    cj4_player self = cj4_state_current_player(state);
    cj4_player_view view = cj4m_make_player_view(state, self);
    assert(cj4dr_encode_input(&view, rgb));
    assert(memcmp(rgb, teacher.rgb, sizeof(rgb)) == 0);
    for (unsigned tile = 0; tile < 136; ++tile) {
        cj4_location location = state->locations[tile];
        if (cj4_location_is_hand(location.placement) &&
            cj4_location_placement_player(location.placement) != self) {
            size_t offset = cj4dr_tile_rgb_offset((cj4_tile_id)tile);
            assert(rgb[offset] == 255 && rgb[offset + 1] == 255 && rgb[offset + 2] == 255);
        }
        if (cj4_can_discard(*state, rules, (cj4_tile_id)tile)) {
            unsigned type = tile / 4;
            legal[type] = 1;
            cj4_mahjong after = cj4_do_discard(*state, rules, (cj4_tile_id)tile);
            bool any = false;
            for (cj4_player p = 0; p < 4; ++p)
                if (p != self && cj4_can_ron(&after, p, rules)) any = true;
            assert(teacher.target[type] == (any ? 255 : 0));
        }
    }
    assert(memcmp(legal, teacher.mask, sizeof(legal)) == 0);
}

int main(void)
{
    cj4_rules rules = cj4_rules_default();
    for (unsigned type = 0; type < 34; ++type)
        for (unsigned seed = 0; seed < 4; ++seed)
            for (unsigned kind = 0; kind < CJ4DR_CASE_COUNT; ++kind) {
                cj4_mahjong state;
                assert(cj4dr_make_case((cj4dr_case)kind, (cj4_tile_type)type, seed, &rules, &state));
                check_sample(&state, &rules);
                cj4dr_teacher sample;
                assert(cj4dr_make_teacher(&state, &rules, &sample));
                assert(sample.mask[type]);
                assert(sample.target[type] == (kind == CJ4DR_CASE_RIICHI_DANGER ? 255 : 0));
                if (kind == CJ4DR_CASE_ALL_GENBUTSU) {
                    for (cj4_player p = 1; p < 4; ++p) {
                        cj4_discard_list discards = cj4_location_collect_player_discards(state.locations, p);
                        assert(discards.count == 1);
                        assert(cj4_tile_get_type(discards.items[0].tile) == type);
                    }
                } else {
                    assert(cj4_state_is_riichi(&state, 1));
                    assert(cj4_state_riichi_furiten(&state, 1) == (kind == CJ4DR_CASE_RIICHI_PASSED));
                }
            }
    cj4_mahjong danger;
    assert(cj4dr_make_case(CJ4DR_CASE_RIICHI_DANGER, 4, 10, &rules, &danger));
    cj4dr_teacher before, after;
    assert(cj4dr_make_teacher(&danger, &rules, &before));
    assert(before.target[4] == 255);
    /* Hidden furiten changes the teacher, never the RGB input. */
    danger.furiten |= (CJ4_STATE_RIICHI_FURITEN_FLAG << 1);
    assert(cj4dr_make_teacher(&danger, &rules, &after));
    assert(after.target[4] == 0 && !memcmp(before.rgb, after.rgb, sizeof(before.rgb)));

    /* A riichi player can only discard the draw: other held types remain unlabelled. */
    assert(cj4dr_make_case(CJ4DR_CASE_RIICHI_DANGER, 4, 10, &rules, &danger));
    danger = cj4_do_discard(danger, &rules, danger.draw_tile);
    danger = cj4_do_pass(danger, &rules);
    assert(cj4_state_current_player(&danger) == 1);
    assert(cj4dr_make_teacher(&danger, &rules, &after));
    unsigned count = 0;
    for (unsigned t = 0; t < 34; ++t) count += after.mask[t];
    assert(count == 1 && after.mask[cj4_tile_get_type(danger.draw_tile)]);
    check_sample(&danger, &rules);

    cj4dr_teacher unchanged;
    memset(&after, 0xa5, sizeof(after));
    unchanged = after;
    danger = cj4_do_discard(danger, &rules, danger.draw_tile);
    assert(!cj4dr_make_teacher(&danger, &rules, &after));
    assert(!memcmp(&after, &unchanged, sizeof(after)));
    assert(!cj4dr_make_teacher(NULL, &rules, &after));
    assert(!cj4dr_make_case(CJ4DR_CASE_COUNT, 0, 1, &rules, &danger));
    puts("Teacher: all tile types, legal replay, masked input, ron/furiten and legal masks passed");
    return 0;
}
