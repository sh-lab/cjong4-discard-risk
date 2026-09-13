#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cjong4/core/state_init.h"
#include "cjong4/core/state_query.h"
#include "cjong4/manager/manager.h"
#include "cjong4_discard_risk/input.h"

_Static_assert(CJ4DR_INPUT_SCHEMA_VERSION == 1, "update schema fixtures");
_Static_assert(CJ4DR_INPUT_SIZE == 414, "wire size changed");

static const cj4_tile_id no_dora[5] = {255, 255, 255, 255, 255};

static cj4_player_view empty_view(cj4_player observer)
{
    cj4_player_view view;
    memset(&view, 0, sizeof(view));
    memset(view.locations, 255, sizeof(view.locations));
    view.player = observer;
    return view;
}

static void assert_unchanged(const uint8_t *output)
{
    for (size_t i = 0; i < 414; ++i)
        assert(output[i] == 0xa5);
}

static void test_wire_fixture(void)
{
    cj4_player_view view = empty_view(2);
    const cj4_tile_id indicators[5] = {135, 255, 20, 0, 255};
    uint8_t output[416], expected[414];
    /* Self hand; called riichi/tsumogiri discard owned by 3, pon by 1. */
    view.locations[7].placement = 0x40;
    view.locations[80].discard = 0xe3;
    view.locations[80].placement = 0xb1;
    view.locations[80].discard_history = 0x91;
    cj4_player_view original = view;
    memset(expected, 255, sizeof(expected));
    expected[22] = 0x00;
    expected[240] = 0xa3;
    expected[241] = 0xf1;
    expected[242] = 0x91;
    expected[408] = 0;
    expected[409] = 20;
    expected[410] = 135;
    expected[413] = 7;
    memset(output, 0xa5, sizeof(output));
    assert(cj4dr_encode_input(&view, indicators, 7, output + 1));
    assert(output[0] == 0xa5 && output[415] == 0xa5);
    assert(memcmp(output + 1, expected, 414) == 0);
    assert(cj4dr_validate_input(output + 1, 414));
    assert(memcmp(&view, &original, sizeof(view)) == 0);
    assert(indicators[0] == 135 && indicators[1] == 255 && indicators[2] == 20);
}

static void test_rotation_and_ignored_fields(void)
{
    uint8_t baseline[414], output[414];
    for (cj4_player observer = 0; observer < 4; ++observer) {
        cj4_player_view view = empty_view(observer);
        view.locations[0].placement = (uint8_t)(observer * 32);
        for (unsigned relative = 0; relative < 4; ++relative) {
            unsigned owner = (observer + relative) % 4;
            view.locations[relative + 1].discard = (uint8_t)(owner * 32 + 158);
            view.locations[relative + 1].discard_history = 213;
            /* Every meld type and group must survive relative conversion. */
            for (unsigned group = 0; group < 4; ++group)
                for (unsigned type = 0; type < 5; ++type)
                    view.locations[10 + relative * 20 + group * 5 + type].placement =
                        (uint8_t)(128 + owner * 32 + group * 8 + type);
        }
        assert(cj4dr_encode_input(&view, no_dora, 0, output));
        assert(cj4dr_validate_input(output, sizeof(output)));
        if (observer == 0)
            memcpy(baseline, output, sizeof(output));
        else
            assert(memcmp(baseline, output, sizeof(output)) == 0);

        /* Everything other than locations and observer is out of schema. */
        cj4_player_view altered;
        memset(&altered, 0xa5, sizeof(altered));
        altered.player = observer;
        memcpy(altered.locations, view.locations, sizeof(view.locations));
        for (size_t tile = 0; tile < 136; ++tile)
            altered.locations[tile].wall = (uint8_t)tile;
        assert(cj4dr_encode_input(&altered, no_dora, 0, output));
        assert(memcmp(baseline, output, sizeof(output)) == 0);
    }
}

static void test_indicator_normalization(void)
{
    cj4_player_view view = empty_view(0);
    uint8_t output[414], baseline[414];
    for (unsigned count = 0; count <= 5; ++count) {
        cj4_tile_id dora[5] = {255, 255, 255, 255, 255};
        for (unsigned i = 0; i < count; ++i)
            dora[i] = (uint8_t)(135 - i);
        assert(cj4dr_encode_input(&view, dora, 135, baseline));
        assert(cj4dr_validate_input(baseline, sizeof(baseline)));
        for (unsigned i = 0; i < 5; ++i)
            assert(baseline[408 + i] == (i < count ? 136 - count + i : 255));
        for (unsigned rotation = 0; rotation < 5; ++rotation) {
            cj4_tile_id permuted[5];
            for (unsigned i = 0; i < 5; ++i)
                permuted[i] = dora[(i + rotation) % 5];
            assert(cj4dr_encode_input(&view, permuted, 135, output));
            assert(memcmp(baseline, output, sizeof(output)) == 0);
        }
    }
    /* Different physical copies of one indicator type are allowed. */
    const cj4_tile_id same_type[5] = {3, 1, 2, 0, 255};
    assert(cj4dr_encode_input(&view, same_type, 0, output));
    for (unsigned i = 0; i < 4; ++i)
        assert(output[408 + i] == i);

    const cj4_tile_id duplicates[5] = {1, 255, 1, 255, 255};
    memset(output, 0xa5, sizeof(output));
    assert(!cj4dr_encode_input(&view, duplicates, 0, output));
    assert_unchanged(output);
}

static void test_candidate_identity(void)
{
    cj4_player_view view = empty_view(0);
    uint8_t baseline[414], output[414];
    assert(cj4dr_encode_input(&view, no_dora, 0, baseline));
    for (unsigned tile = 0; tile < 136; ++tile) {
        assert(cj4dr_encode_input(&view, no_dora, (uint8_t)tile, output));
        assert(memcmp(baseline, output, 413) == 0);
        assert(output[413] == tile);
        assert(cj4dr_validate_input(output, sizeof(output)));
    }
}

static void test_all_field_bytes(void)
{
    for (unsigned field = 0; field < 3; ++field) {
        for (unsigned byte = 0; byte < 256; ++byte) {
            cj4_player_view view = empty_view(0);
            uint8_t output[414], input[414];
            bool expected;
            if (field == 0) {
                view.locations[135].discard = (uint8_t)byte;
                expected = byte == 255 || byte % 32 <= 30;
            } else if (field == 1) {
                view.locations[135].placement = (uint8_t)byte;
                expected = byte == 255 || byte == 0 ||
                           (byte >= 128 && byte % 8 <= 4);
            } else {
                view.locations[135].discard_history = (uint8_t)byte;
                expected = byte == 255 || byte % 128 <= 85;
            }
            memset(output, 0xa5, sizeof(output));
            assert(cj4dr_encode_input(&view, no_dora, 0, output) == expected);
            if (!expected)
                assert_unchanged(output);
            memset(input, 255, sizeof(input));
            input[413] = 0;
            input[405 + field] = (uint8_t)byte;
            assert(cj4dr_validate_input(input, sizeof(input)) == expected);
        }
    }
    for (unsigned byte = 0; byte < 256; ++byte) {
        cj4_player_view view = empty_view(0);
        cj4_tile_id dora[5] = {(uint8_t)byte, 255, 255, 255, 255};
        uint8_t output[414], input[414];
        memset(output, 0xa5, sizeof(output));
        bool dora_valid = byte < 136 || byte == 255;
        assert(cj4dr_encode_input(&view, dora, 0, output) == dora_valid);
        if (!dora_valid)
            assert_unchanged(output);
        memset(input, 255, sizeof(input));
        input[408] = (uint8_t)byte;
        input[413] = 0;
        assert(cj4dr_validate_input(input, sizeof(input)) == dora_valid);
        memset(output, 0xa5, sizeof(output));
        assert(cj4dr_encode_input(&view, no_dora, (uint8_t)byte, output) == (byte < 136));
        if (byte >= 136)
            assert_unchanged(output);
        input[408] = 255;
        input[413] = (uint8_t)byte;
        assert(cj4dr_validate_input(input, sizeof(input)) == (byte < 136));
    }
}

static void test_invalid_arguments_and_wire(void)
{
    cj4_player_view view = empty_view(0);
    uint8_t output[414], input[414];
    memset(output, 0xa5, sizeof(output));
    assert(!cj4dr_encode_input(NULL, no_dora, 0, output));
    assert(!cj4dr_encode_input(&view, NULL, 0, output));
    assert(!cj4dr_encode_input(&view, no_dora, 0, NULL));
    for (unsigned player = 4; player < 256; ++player) {
        view.player = (uint8_t)player;
        assert(!cj4dr_encode_input(&view, no_dora, 0, output));
    }
    assert_unchanged(output);
    for (cj4_player observer = 0; observer < 4; ++observer) {
        view = empty_view(observer);
        for (cj4_player owner = 0; owner < 4; ++owner) {
            view.locations[135].placement = (uint8_t)(owner * 32);
            memset(output, 0xa5, sizeof(output));
            assert(cj4dr_encode_input(&view, no_dora, 0, output) == (owner == observer));
            if (owner != observer)
                assert_unchanged(output);
        }
    }
    assert(!cj4dr_validate_input(NULL, 414));
    const uint8_t short_input = 255;
    assert(!cj4dr_validate_input(&short_input, 0));
    assert(!cj4dr_validate_input(&short_input, 1));
    assert(!cj4dr_validate_input(&short_input, 413));
    assert(!cj4dr_validate_input(&short_input, 415));
    memset(input, 255, sizeof(input));
    input[413] = 0;
    input[409] = 0; /* Gap after an unused indicator slot. */
    assert(!cj4dr_validate_input(input, sizeof(input)));
    input[408] = 1; /* Descending IDs. */
    assert(!cj4dr_validate_input(input, sizeof(input)));
    input[409] = 1; /* Duplicate physical ID. */
    assert(!cj4dr_validate_input(input, sizeof(input)));
}

static void test_cjong4_integration(void)
{
    cj4_tile_id wall[136];
    cj4_rules rules = cj4_rules_default();
    for (unsigned i = 0; i < 136; ++i)
        wall[i] = (uint8_t)i;
    cj4_mahjong state = cj4_create_initial_state(wall, &rules);
    cj4_action actions[CJ4M_MAX_ACTIONS];
    unsigned tested = 0;
    for (cj4_player observer = 0; observer < 4; ++observer) {
        cj4_player_view view = cj4m_make_player_view(&state, observer);
        cj4_dora_indicator_list dora = cj4_location_collect_dora_indicators(view.locations);
        assert(dora.count == 1 && dora.items[0] == 130);
        unsigned count = cj4m_collect_actions(&state, &rules, observer,
                                              actions, CJ4M_MAX_ACTIONS);
        for (unsigned i = 0; i < count; ++i) {
            if (actions[i].type != CJ4_ACTION_DISCARD)
                continue;
            uint8_t input[414];
            assert(cj4dr_encode_input(&view, dora.items, actions[i].tile, input));
            assert(cj4dr_validate_input(input, sizeof(input)));
            ++tested;
        }
        uint8_t input[414];
        cj4_hand hand = cj4_location_collect_hand(view.locations, observer);
        assert(hand.count > 0);
        assert(cj4dr_encode_input(&view, dora.items, hand.items[0], input));
        for (unsigned tile = 0; tile < 136; ++tile) {
            if (cj4_location_is_hand(state.locations[tile].placement)) {
                cj4_player owner = cj4_location_placement_player(state.locations[tile].placement);
                assert(input[tile * 3 + 1] == (owner == observer ? 0 : 255));
            }
        }
        assert(input[408] == 130 && input[409] == 255);
        /* Copying the full state into a view is rejected. */
        memcpy(view.locations, state.locations, sizeof(view.locations));
        assert(!cj4dr_encode_input(&view, dora.items, hand.items[0], input));
    }
    assert(tested > 0);
}

int main(void)
{
    test_wire_fixture();
    test_rotation_and_ignored_fields();
    test_indicator_normalization();
    test_candidate_identity();
    test_all_field_bytes();
    test_invalid_arguments_and_wire();
    test_cjong4_integration();
    puts("All discard-risk input tests passed.");
    return 0;
}
