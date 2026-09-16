#ifndef CJ4DR_INPUT_H
#define CJ4DR_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cjong4/manager/player_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* v3: five row-major RGB images only. Persist the schema version separately.
 * v1 (414 bytes) and v2 (409 bytes with candidate ID) are incompatible. */
#define CJ4DR_INPUT_SCHEMA_VERSION 3u

enum {
    CJ4DR_LOCATION_STRIDE = 3,
    CJ4DR_DISCARD_OFFSET = 0,          /* R */
    CJ4DR_PLACEMENT_OFFSET = 1,        /* G */
    CJ4DR_DISCARD_HISTORY_OFFSET = 2,  /* B */
    CJ4DR_IMAGE_HEIGHT = 4,
    CJ4DR_RGB_SIZE = 408,
    CJ4DR_INPUT_SIZE = CJ4DR_RGB_SIZE
};

typedef enum {
    CJ4DR_IMAGE_MANZU = 0,
    CJ4DR_IMAGE_PINZU,
    CJ4DR_IMAGE_SOUZU,
    CJ4DR_IMAGE_WINDS,
    CJ4DR_IMAGE_DRAGONS,
    CJ4DR_IMAGE_COUNT
} cj4dr_image;

typedef struct {
    uint16_t byte_offset;
    uint8_t width;
    uint8_t height;
    cj4_tile_type first_type;
} cj4dr_image_layout;

/* Widths 9,9,9,4,3; x is tile type, y is physical copy (0..3).
 * RGB bytes are interleaved, rows packed, no padding between images.
 * Failure leaves out_layout unchanged. */
bool cj4dr_get_image_layout(cj4dr_image image, cj4dr_image_layout *out_layout);
cj4_tile_id cj4dr_image_tile_id(cj4dr_image image, uint8_t x, uint8_t y);
/* Returns SIZE_MAX for an invalid tile ID. */
size_t cj4dr_tile_rgb_offset(cj4_tile_id tile);

/* Encode a masked view. Only player bits are relativized to the observer.
 * RGB values are otherwise unchanged. No dora, wall, tile aggregation, color
 * processing, or inferred flags. No candidate ID is accepted. The caller decides
 * which outputs correspond to legal discards. Full state consistency is not checked.
 * Invalid field encodings/opponent hands are rejected. Requires a non-NULL
 * view and at least 408 writable output bytes. Failure leaves output unchanged.
 * No allocation. Sources and outputs must not overlap. */
bool cj4dr_encode_input(const cj4_player_view *view,
                       uint8_t out_input[CJ4DR_INPUT_SIZE]);

/* Exact v3 byte length and field validation; NULL/wrong size is not read.
 * Canonical hand owners must be 0. Does not validate game legality. */
bool cj4dr_validate_input(const uint8_t *input, size_t size);

/* Recover all relative locations losslessly; wall is set to 255.
 * Output is required and must not overlap the input.
 * Failure leaves output unchanged. */
bool cj4dr_decode_input(const uint8_t *input, size_t size,
                       cj4_location out_locations[CJ4_TILE_ID_COUNT]);

#ifdef __cplusplus
}
#endif
#endif /* CJ4DR_INPUT_H */
