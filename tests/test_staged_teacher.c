#include "staged_teacher.h"
#include "cjong4/core/state_discard.h"
#include "cjong4/core/state_pass.h"
#include "cjong4/core/state_query.h"
#include <assert.h>
#include <string.h>

static cj4_player_view empty_view(void)
{
    cj4_player_view view;
    memset(&view, 0, sizeof(view));
    memset(view.locations, 255, sizeof(view.locations));
    view.player = 0;
    view.live_wall_remaining = 69;
    return view;
}

/* Public-layout fixtures isolate the scoring table from hand legality. */
static void meld(cj4_player_view *view, unsigned group, cj4_meld_type kind)
{
    unsigned first = 40 + group * 4;
    for (unsigned i = 0; i < (kind == CJ4_MELD_ANKAN ? 4u : 3u); ++i)
        view->locations[first + i].placement =
            (uint8_t)(0x80 | (1 << 5) | (group << 3) | kind);
}

void test_staged_teacher(void)
{
    cj4dr_safety safety = {0};
    cj4_player_view view = empty_view();
    const unsigned remaining[] = {69, 46, 45, 22, 21, 0};
    const uint8_t expected[][4] = {
        {0,16,32,64}, {0,16,32,64}, {0,64,96,128},
        {0,64,96,128}, {0,128,160,192}, {0,128,160,192}
    };
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned unknown = 0; unknown <= 3; ++unknown) {
            view.live_wall_remaining = (uint8_t)remaining[i];
            memset(&safety, 0, sizeof(safety));
            for (unsigned p = unknown + 1; p < 4; ++p)
                safety.passed[p][4] = 1;
            assert(cj4dr_stage_score(&view, &safety, 4) == expected[i][unknown]);
        }

    memset(&safety, 0, sizeof(safety));
    view = empty_view();
    for (unsigned open = 1; open <= 4; ++open) {
        meld(&view, open - 1, CJ4_MELD_PON);
        assert(cj4dr_stage_score(&view, &safety, 4) == (open < 3 ? 128 : 192));
    }
    safety.passed[1][4] = 1;
    assert(cj4dr_stage_score(&view, &safety, 4) == 32); /* safe threat excluded */
    view = empty_view();
    memset(&safety, 0, sizeof(safety));
    meld(&view, 0, CJ4_MELD_ANKAN);
    assert(cj4dr_stage_score(&view, &safety, 4) == 64);
    meld(&view, 1, CJ4_MELD_KAKAN);
    assert(cj4dr_stage_score(&view, &safety, 4) == 128);
    view.is_riichi[2] = 1;
    assert(cj4dr_stage_score(&view, &safety, 4) == 192);

    /* Genbutsu for all three remains 0 even with threats and no live draws.
     * A called-away discard retains its permanent safety. */
    view.live_wall_remaining = 0;
    for (unsigned p = 1; p < 4; ++p) {
        view.locations[16 + p].discard = (uint8_t)(p << 5);
        view.locations[16 + p].discard_history = (uint8_t)(p - 1);
    }
    view.locations[17].placement = (uint8_t)(0x80 | (2 << 5) | CJ4_MELD_PON);
    assert(cj4dr_stage_score(&view, &safety, 4) == 0);

    cj4_rules rules = cj4_rules_default();
    cj4_mahjong state;
    assert(cj4dr_make_case(CJ4DR_CASE_RIICHI_DANGER, 4, 10, &rules, &state));
    memset(&safety, 0, sizeof(safety));
    cj4dr_teacher staged, oracle, hidden;
    assert(cj4dr_make_staged_teacher(&state, &rules, &safety, &staged));
    assert(cj4dr_make_teacher(&state, &rules, &oracle));
    assert(oracle.target[4] == 255 && staged.target[4] == 192);
    assert(!memcmp(staged.rgb, oracle.rgb, sizeof(staged.rgb)));
    assert(!memcmp(staged.mask, oracle.mask, sizeof(staged.mask)));
    state.furiten ^= 0xff;
    assert(cj4dr_make_staged_teacher(&state, &rules, &safety, &hidden));
    assert(!memcmp(&staged, &hidden, sizeof(staged))); /* no hidden flags in scores */
    uint8_t targets[34];
    memcpy(targets, staged.target, sizeof(targets));
    assert(cj4dr_mark_actual_ron(&staged, 16));
    assert(staged.target[4] == 255);
    for (unsigned t = 0; t < 34; ++t)
        if (t != 4) assert(staged.target[t] == targets[t]);
    assert(!memcmp(staged.rgb, oracle.rgb, sizeof(staged.rgb)));
    assert(!cj4dr_mark_actual_ron(&staged, -1));
    assert(!cj4dr_mark_actual_ron(&staged, 136));
    assert(!cj4dr_mark_actual_ron(NULL, 16));
    for (unsigned t = 0; t < 34; ++t)
        if (!staged.mask[t]) assert(!cj4dr_mark_actual_ron(&staged, (int)t * 4));
    assert(!cj4dr_make_staged_teacher(NULL, &rules, &safety, &staged));

    /* Real discard/pass transitions: temporary safety expires at the player's
     * next draw, while riichi passed-tile evidence survives their next draw. */
    for (unsigned riichi = 0; riichi < 2; ++riichi) {
        assert(cj4dr_make_case(riichi ? CJ4DR_CASE_RIICHI_DANGER : CJ4DR_CASE_ALL_GENBUTSU,
                               4, 10, &rules, &state));
        cj4_tile_id tile = state.draw_tile;
        unsigned type = cj4_tile_get_type(tile);
        cj4_mahjong discarded = cj4_do_discard(state, &rules, tile);
        cj4_mahjong after = cj4_do_pass(discarded, &rules);
        memset(&safety, 0, sizeof(safety));
        cj4dr_safety_observe(&safety, &state, &discarded);
        assert(!safety.passed[2][type]); /* discard not resolved yet */
        cj4dr_safety_observe(&safety, &discarded, &after);
        assert(safety.passed[1][type] == riichi);
        assert(safety.passed[2][type] && safety.passed[3][type]);

        cj4_mahjong next = after;
        next.progress = (uint8_t)((next.progress & ~CJ4_STATE_CURRENT_PLAYER_MASK) | (2 << 4));
        cj4dr_safety_observe(&safety, &after, &next);
        assert(!safety.passed[2][type]); /* next normal draw */
        assert(safety.passed[3][type]);
        next.wall_pos = 122; /* houtei: new yaku may invalidate non-riichi evidence */
        cj4dr_safety_observe(&safety, &after, &next);
        assert(!safety.passed[3][type]);
        assert(safety.passed[1][type] == riichi);

        memset(&safety, 1, sizeof(safety));
        next = after;
        next.progress = (uint8_t)((2 << 4) | CJ4_PHASE_AFTER_CALL);
        next.locations[tile].placement = (uint8_t)(0x80 | (2 << 5) | CJ4_MELD_PON);
        cj4dr_safety_observe(&safety, &discarded, &next);
        assert(!safety.passed[2][type]); /* caller's hand changed */
        memset(&safety, 0, sizeof(safety));
        cj4dr_safety_observe(&safety, &discarded, &next);
        assert(!safety.passed[1][type]); /* competing call isn't a complete pass */
        next.progress = (uint8_t)((2 << 4) | CJ4_PHASE_DRAW);
        cj4dr_safety_observe(&safety, &discarded, &next);
        assert(!safety.passed[1][type]); /* same for minkan/rinshan */

        memset(&safety, 1, sizeof(safety));
        next = after;
        next.locations[40].placement = (uint8_t)(0x80 | (1 << 5) | CJ4_MELD_ANKAN);
        cj4dr_safety_observe(&safety, &after, &next);
        assert(!safety.passed[1][type]); /* conservative invalidation on kan */
        next.discard_count = 0;
        cj4dr_safety_observe(&safety, &after, &next);
        cj4dr_safety zero = {0};
        assert(!memcmp(&safety, &zero, sizeof(safety)));
    }
}

#ifdef CJ4DR_TEST_STANDARD
#include "cjong4/opponent/opponent_standard.h"
#include "cjong4/core/state_init.h"
#include "cjong4/core/state_round.h"
#include "cjong4/core/state_ron.h"

void test_staged_selfplay(void)
{
    cj4_rules rules = cj4_rules_default();
    uint64_t rng = 82000;
    rng = cj4dr_random(&rng);
    cj4_tile_id wall[136];
    cj4dr_shuffle(wall, &rng);
    cj4_mahjong state = cj4_create_initial_state(wall, &rules);
    cj4m_player_delegate delegates[4];
    for (unsigned p = 0; p < 4; ++p) delegates[p] = cj4_opponent_standard(1);
    cj4dr_safety safety = {0};
    unsigned checked = 0;
    for (unsigned step = 0; step < 10000; ++step) {
        cj4_phase phase = cj4_state_phase(&state);
        if (phase == CJ4_PHASE_GAME_END) {
            assert(checked > 0);
            return;
        }
        if (phase == CJ4_PHASE_DRAW || phase == CJ4_PHASE_AFTER_CALL) {
            cj4dr_teacher sample;
            assert(cj4dr_make_staged_teacher(&state, &rules, &safety, &sample));
            for (unsigned id = 0; id < 136; ++id) {
                if (!cj4_can_discard(state, &rules, (cj4_tile_id)id)) continue;
                unsigned type = id / 4;
                assert(sample.mask[type] && sample.target[type] != 255);
                cj4_mahjong after = cj4_do_discard(state, &rules, (cj4_tile_id)id);
                cj4_player self = cj4_state_current_player(&state);
                for (cj4_player p = 0; p < 4; ++p)
                    if (p != self && (sample.target[type] == 0 || safety.passed[p][type])) {
                        /* Audit with the hidden-state oracle only in this test.
                         * Production staged labels never call this oracle. */
                        assert(!cj4_can_ron(&after, p, &rules));
                        ++checked;
                    }
            }
        }
        cj4_mahjong next;
        if (phase == CJ4_PHASE_SETTLE && !cj4_can_game_end(state)) {
            assert(cj4_can_next_round(state));
            cj4dr_shuffle(wall, &rng);
            next = cj4_do_next_round(state, wall, &rules);
        } else {
            next = cj4m_step(&state, &rules, delegates);
        }
        assert(memcmp(&state, &next, sizeof(state)));
        cj4dr_safety_observe(&safety, &state, &next);
        state = next;
    }
    assert(!"self-play step limit reached");
}
#endif
