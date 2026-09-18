#include "teacher.h"

#include "cjong4/core/state_discard.h"
#include "cjong4/core/state_init.h"
#include "cjong4/core/state_pass.h"
#include "cjong4/core/state_query.h"
#include "cjong4/core/state_riichi.h"
#include "cjong4/core/state_ron.h"
#include <string.h>

uint64_t cj4dr_random(uint64_t *rng)
{
    /* SplitMix64: explicit unsigned arithmetic, reproducible across hosts. */
    uint64_t z = (*rng += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

uint32_t cj4dr_uniform(uint64_t *rng, uint32_t bound)
{
    uint64_t threshold = (UINT64_C(0) - bound) % bound;
    uint64_t value;
    do { value = cj4dr_random(rng); } while (value < threshold);
    return (uint32_t)(value % bound);
}

void cj4dr_shuffle(cj4_tile_id wall[136], uint64_t *rng)
{
    for (unsigned i = 0; i < 136; ++i)
        wall[i] = (cj4_tile_id)i;
    for (unsigned i = 135; i > 0; --i) {
        unsigned j = cj4dr_uniform(rng, i + 1);
        cj4_tile_id temporary = wall[i];
        wall[i] = wall[j];
        wall[j] = temporary;
    }
}

bool cj4dr_make_teacher(const cj4_mahjong *state, const cj4_rules *rules,
                       cj4dr_teacher *output)
{
    if (!state || !rules || !output || !cj4_rules_validate(rules) ||
        (cj4_state_phase(state) != CJ4_PHASE_DRAW &&
         cj4_state_phase(state) != CJ4_PHASE_AFTER_CALL))
        return false;
    cj4dr_teacher result;
    memset(&result, 0, sizeof(result));
    cj4_player self = cj4_state_current_player(state);
    cj4_player_view view = cj4m_make_player_view(state, self);
    if (!cj4dr_encode_input(&view, result.rgb))
        return false;
    unsigned labels = 0;
    for (unsigned id = 0; id < 136; ++id) {
        cj4_tile_id tile = (cj4_tile_id)id;
        if (!cj4_can_discard(*state, rules, tile))
            continue;
        cj4_tile_type type = cj4_tile_get_type(tile);
        cj4_mahjong after = cj4_do_discard(*state, rules, tile);
        if (cj4_state_phase(&after) != CJ4_PHASE_DISCARD ||
            cj4_get_last_discard_tile(&after) != tile)
            return false;
        uint8_t danger = 0;
        for (cj4_player p = 0; p < CJ4_PLAYER_COUNT; ++p)
            if (p != self && cj4_can_ron(&after, p, rules))
                danger = 255;
        /* The output is per type; detect an unexpected physical-copy conflict. */
        if (result.mask[type] && result.target[type] != danger)
            return false;
        if (!result.mask[type])
            ++labels;
        result.mask[type] = 1;
        result.target[type] = danger;
    }
    if (!labels)
        return false;
    *output = result;
    return true;
}

const char *cj4dr_case_name(cj4dr_case kind)
{
    static const char *const names[] = {
        "all-genbutsu", "riichi-passed", "riichi-danger"
    };
    return (unsigned)kind < CJ4DR_CASE_COUNT ? names[kind] : "invalid";
}

static void place(cj4_tile_id wall[136], unsigned position, cj4_tile_id tile)
{
    for (unsigned i = 0; i < 136; ++i)
        if (wall[i] == tile) {
            wall[i] = wall[position];
            wall[position] = tile;
            return;
        }
}

static bool discard_and_pass(cj4_mahjong *state, const cj4_rules *rules,
                             cj4_tile_id tile)
{
    if (!cj4_can_discard(*state, rules, tile))
        return false;
    *state = cj4_do_discard(*state, rules, tile);
    if (!cj4_can_pass(*state))
        return false;
    *state = cj4_do_pass(*state, rules);
    return cj4_state_phase(state) == CJ4_PHASE_DRAW;
}

bool cj4dr_make_case(cj4dr_case kind, cj4_tile_type type, uint64_t seed,
                    const cj4_rules *rules, cj4_mahjong *output)
{
    if ((unsigned)kind >= CJ4DR_CASE_COUNT || (unsigned)type >= 34 || !rules ||
        !output || !cj4_rules_validate(rules))
        return false;
    uint64_t rng = seed;
    cj4_tile_id wall[136];
    cj4dr_shuffle(wall, &rng);
    unsigned copy = cj4dr_uniform(&rng, 4);
    cj4_tile_id own = (cj4_tile_id)(type * 4 + copy);
    cj4_tile_id other[3];
    for (unsigned i = 0; i < 3; ++i)
        other[i] = (cj4_tile_id)(type * 4 + (copy + i + 1) % 4);
    /* Deal positions: p0=0..3,16..19,32..35,48; p1=4..7,...,49. */
    place(wall, 0, own);
    place(wall, 4, other[0]);
    place(wall, 8, other[1]);
    place(wall, 12, other[2]);
    if (kind != CJ4DR_CASE_ALL_GENBUTSU) {
        /* Six distinct pairs plus the target singleton: a real seven-pairs wait.
         * All three templates retain the other physical copies in opponents' hands. */
        unsigned positions[] = {5, 6, 7, 20, 21, 22, 23, 36, 37, 38, 39, 49};
        unsigned types[33], count = 0;
        for (unsigned t = 0; t < 34; ++t)
            if (t != type)
                types[count++] = t;
        for (unsigned i = 32; i > 0; --i) {
            unsigned j = cj4dr_uniform(&rng, i + 1), tmp = types[i];
            types[i] = types[j]; types[j] = tmp;
        }
        /* Two honor pairs rule out competing standard-hand waits. */
        for (unsigned i = 0; i < 2; ++i)
            for (unsigned j = i; j < 33; ++j)
                if (types[j] >= 27) {
                    unsigned tmp = types[i]; types[i] = types[j]; types[j] = tmp;
                    break;
                }
        for (unsigned i = 0; i < 6; ++i) {
            unsigned c = cj4dr_uniform(&rng, 4);
            place(wall, positions[i * 2], (cj4_tile_id)(types[i] * 4 + c));
            place(wall, positions[i * 2 + 1], (cj4_tile_id)(types[i] * 4 + (c + 1) % 4));
        }
    }
    if (!cj4_wall_is_valid(wall))
        return false;
    cj4_mahjong state = cj4_create_initial_state(wall, rules);
    /* All copies of the target were dealt; these draws cannot be the target. */
    if (!discard_and_pass(&state, rules, state.draw_tile))
        return false;
    if (kind == CJ4DR_CASE_ALL_GENBUTSU) {
        if (!discard_and_pass(&state, rules, other[0]))
            return false;
    } else {
        if (!cj4_can_riichi(&state, state.draw_tile))
            return false;
        state = cj4_do_riichi(state, state.draw_tile);
        state = cj4_do_pass(state, rules);
        if (!cj4_state_is_riichi(&state, 1))
            return false;
    }
    for (unsigned p = 2; p < 4; ++p) {
        cj4_tile_id discard = kind == CJ4DR_CASE_RIICHI_DANGER
            ? state.draw_tile : other[p - 1];
        if (!discard_and_pass(&state, rules, discard))
            return false;
    }
    if (cj4_state_current_player(&state) != 0 || !cj4_can_discard(state, rules, own))
        return false;
    cj4dr_teacher teacher;
    if (!cj4dr_make_teacher(&state, rules, &teacher) || !teacher.mask[type] ||
        teacher.target[type] != (kind == CJ4DR_CASE_RIICHI_DANGER ? 255 : 0))
        return false;
    *output = state;
    return true;
}
