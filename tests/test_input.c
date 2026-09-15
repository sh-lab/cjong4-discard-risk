#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cjong4/core/state_init.h"
#include "cjong4/core/state_query.h"
#include "cjong4/manager/manager.h"
#include "cjong4_discard_risk/input.h"

_Static_assert(CJ4DR_INPUT_SCHEMA_VERSION == 2, "update schema fixtures");
_Static_assert(CJ4DR_INPUT_SIZE == 409, "wire size changed");


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
    for (size_t i = 0; i < 409; ++i)
        assert(output[i] == 0xa5);
}

static void test_wire_fixture(void)
{
    cj4_player_view view = empty_view(2);
    uint8_t output[411], expected[409];
    /* Self hand; called riichi/tsumogiri discard owned by 3, pon by 1. */
    view.locations[7].placement = 0x40;
    view.locations[80].discard = 0xe3;
    view.locations[80].placement = 0xb1;
    view.locations[80].discard_history = 0x91;
    cj4_player_view original;
    memcpy(&original, &view, sizeof(view));
    memset(expected, 255, sizeof(expected));
    expected[85] = 0x00;
    expected[222] = 0xa3;
    expected[223] = 0xf1;
    expected[224] = 0x91;
    expected[408] = 7;
    memset(output, 0xa5, sizeof(output));
    assert(cj4dr_encode_input(&view, 7, output + 1));
    assert(output[0] == 0xa5 && output[410] == 0xa5);
    assert(memcmp(output + 1, expected, 409) == 0);
    assert(cj4dr_validate_input(output + 1, 409));
    assert(memcmp(&view, &original, sizeof(view)) == 0);
}

static void test_rotation_and_ignored_fields(void)
{
    uint8_t baseline[409], output[409];
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
        assert(cj4dr_encode_input(&view, 0, output));
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
        assert(cj4dr_encode_input(&altered, 0, output));
        assert(memcmp(baseline, output, sizeof(output)) == 0);
    }
}

static void test_candidate_identity(void)
{
    cj4_player_view view = empty_view(0);
    uint8_t baseline[409], output[409];
    assert(cj4dr_encode_input(&view, 0, baseline));
    for (unsigned tile = 0; tile < 136; ++tile) {
        assert(cj4dr_encode_input(&view, (uint8_t)tile, output));
        assert(memcmp(baseline, output, 408) == 0);
        assert(output[408] == tile);
        assert(cj4dr_validate_input(output, sizeof(output)));
    }
}

static void test_image_layout_and_roundtrip(void)
{
    const unsigned widths[] = {9, 9, 9, 4, 3};
    const unsigned first_types[] = {0, 9, 18, 27, 31};
    const unsigned offsets[] = {0, 108, 216, 324, 372};
    unsigned seen[136] = {0};
    cj4_player_view view = empty_view(0), restored = empty_view(0);
    uint8_t input[409], roundtrip[409];
    cj4_tile_id candidate = 255;
    for (unsigned image = 0; image < 5; ++image) {
        cj4dr_image_layout layout;
        assert(cj4dr_get_image_layout((cj4dr_image)image, &layout));
        assert(layout.width == widths[image] && layout.height == 4);
        assert(layout.first_type == first_types[image] && layout.byte_offset == offsets[image]);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < widths[image]; ++x) {
                cj4_tile_id tile = (cj4_tile_id)((first_types[image] + x) * 4 + y);
                assert(cj4dr_image_tile_id((cj4dr_image)image, (uint8_t)x, (uint8_t)y) == tile);
                assert(cj4dr_tile_rgb_offset(tile) == offsets[image] + (y * widths[image] + x) * 3);
                ++seen[tile];
                /* Distinct, valid encodings; a structural fixture, not a game. */
                view.locations[tile].discard = (uint8_t)((tile % 4) * 32 + tile % 31);
                view.locations[tile].placement = (uint8_t)(128 + (tile % 4) * 32 +
                                                          ((tile / 4) % 4) * 8 + tile % 5);
                view.locations[tile].discard_history = (uint8_t)(tile % 86 + (tile % 2) * 128);
            }
        assert(cj4dr_image_tile_id((cj4dr_image)image, (uint8_t)widths[image], 0) == 255);
        assert(cj4dr_image_tile_id((cj4dr_image)image, 0, 4) == 255);
    }
    for (unsigned tile = 0; tile < 136; ++tile)
        assert(seen[tile] == 1);
    assert(cj4dr_encode_input(&view, 135, input));
    assert(cj4dr_decode_input(input, sizeof(input), restored.locations, &candidate));
    assert(candidate == 135);
    assert(memcmp(view.locations, restored.locations, sizeof(view.locations)) == 0);
    assert(cj4dr_encode_input(&restored, candidate, roundtrip));
    assert(memcmp(input, roundtrip, sizeof(input)) == 0);
    for (unsigned tile = 136; tile < 256; ++tile)
        assert(cj4dr_tile_rgb_offset((uint8_t)tile) == SIZE_MAX);
    cj4dr_image_layout layout = {17, 18, 19, 20};
    assert(!cj4dr_get_image_layout((cj4dr_image)-1, &layout));
    assert(!cj4dr_get_image_layout(CJ4DR_IMAGE_COUNT, &layout));
    assert(!cj4dr_get_image_layout(CJ4DR_IMAGE_MANZU, NULL));
    assert(layout.byte_offset == 17 && layout.width == 18);
    assert(cj4dr_image_tile_id((cj4dr_image)-1, 0, 0) == 255);
    assert(cj4dr_image_tile_id(CJ4DR_IMAGE_COUNT, 0, 0) == 255);

    input[408] = 255;
    assert(!cj4dr_decode_input(input, sizeof(input), restored.locations, &candidate));
    assert(candidate == 135);
    assert(memcmp(view.locations, restored.locations, sizeof(view.locations)) == 0);
    assert(!cj4dr_decode_input(input, 408, restored.locations, &candidate));
    assert(!cj4dr_decode_input(NULL, 409, restored.locations, &candidate));
    assert(!cj4dr_decode_input(roundtrip, 409, NULL, &candidate));
    assert(!cj4dr_decode_input(roundtrip, 409, restored.locations, NULL));
}

static void test_all_field_bytes(void)
{
    for (unsigned field = 0; field < 3; ++field) {
        for (unsigned byte = 0; byte < 256; ++byte) {
            cj4_player_view view = empty_view(0);
            uint8_t output[409], input[409];
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
            assert(cj4dr_encode_input(&view, 0, output) == expected);
            if (!expected)
                assert_unchanged(output);
            memset(input, 255, sizeof(input));
            input[408] = 0;
            input[405 + field] = (uint8_t)byte;
            assert(cj4dr_validate_input(input, sizeof(input)) == expected);
        }
    }
    for (unsigned byte = 0; byte < 256; ++byte) {
        cj4_player_view view = empty_view(0);
        uint8_t output[409], input[409];
        memset(output, 0xa5, sizeof(output));
        memset(input, 255, sizeof(input));
        assert(cj4dr_encode_input(&view, (uint8_t)byte, output) == (byte < 136));
        if (byte >= 136)
            assert_unchanged(output);
        input[408] = (uint8_t)byte;
        assert(cj4dr_validate_input(input, sizeof(input)) == (byte < 136));
    }
}

static void test_invalid_arguments_and_wire(void)
{
    cj4_player_view view = empty_view(0);
    uint8_t output[409];
    memset(output, 0xa5, sizeof(output));
    assert(!cj4dr_encode_input(NULL, 0, output));
    assert(!cj4dr_encode_input(&view, 0, NULL));
    for (unsigned player = 4; player < 256; ++player) {
        view.player = (uint8_t)player;
        assert(!cj4dr_encode_input(&view, 0, output));
    }
    assert_unchanged(output);
    for (cj4_player observer = 0; observer < 4; ++observer) {
        view = empty_view(observer);
        for (cj4_player owner = 0; owner < 4; ++owner) {
            view.locations[135].placement = (uint8_t)(owner * 32);
            memset(output, 0xa5, sizeof(output));
            assert(cj4dr_encode_input(&view, 0, output) == (owner == observer));
            if (owner != observer)
                assert_unchanged(output);
        }
    }
    assert(!cj4dr_validate_input(NULL, 409));
    const uint8_t short_input = 255;
    assert(!cj4dr_validate_input(&short_input, 0));
    assert(!cj4dr_validate_input(&short_input, 1));
    assert(!cj4dr_validate_input(&short_input, 408));
    assert(!cj4dr_validate_input(&short_input, 410));
    assert(!cj4dr_validate_input(&short_input, 414)); /* old schema */
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
        unsigned count = cj4m_collect_actions(&state, &rules, observer,
                                              actions, CJ4M_MAX_ACTIONS);
        for (unsigned i = 0; i < count; ++i) {
            if (actions[i].type != CJ4_ACTION_DISCARD)
                continue;
            uint8_t input[409];
            assert(cj4dr_encode_input(&view, actions[i].tile, input));
            assert(cj4dr_validate_input(input, sizeof(input)));
            ++tested;
        }
        uint8_t input[409];
        cj4_hand hand = cj4_location_collect_hand(view.locations, observer);
        assert(hand.count > 0);
        assert(cj4dr_encode_input(&view, hand.items[0], input));
        for (unsigned tile = 0; tile < 136; ++tile) {
            if (cj4_location_is_hand(state.locations[tile].placement)) {
                cj4_player owner = cj4_location_placement_player(state.locations[tile].placement);
                assert(input[cj4dr_tile_rgb_offset((cj4_tile_id)tile) + 1] == (owner == observer ? 0 : 255));
            }
        }
        /* Copying the full state into a view is rejected. */
        memcpy(view.locations, state.locations, sizeof(view.locations));
        assert(!cj4dr_encode_input(&view, hand.items[0], input));
    }
    assert(tested > 0);
}

void test_model(void);

int main(void)
{
    test_wire_fixture();
    test_rotation_and_ignored_fields();
    test_candidate_identity();
    test_image_layout_and_roundtrip();
    test_all_field_bytes();
    test_invalid_arguments_and_wire();
    test_cjong4_integration();
    test_model();
    puts("All discard-risk RGB input and INT8 inference tests passed.");
    return 0;
}
