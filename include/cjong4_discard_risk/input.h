#ifndef CJ4DR_INPUT_H
#define CJ4DR_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cjong4/manager/player_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Store this version separately when persisting or exchanging inputs. */
#define CJ4DR_INPUT_SCHEMA_VERSION 1u

enum {
    CJ4DR_LOCATION_STRIDE = 3,
    CJ4DR_DISCARD_OFFSET = 0,
    CJ4DR_PLACEMENT_OFFSET = 1,
    CJ4DR_DISCARD_HISTORY_OFFSET = 2,
    CJ4DR_DORA_OFFSET = 408,
    CJ4DR_DORA_COUNT = 5,
    CJ4DR_CANDIDATE_OFFSET = 413,
    CJ4DR_INPUT_SIZE = 414
};

/**
 * Serialize a masked player view into canonical schema-v1 bytes.
 *
 * Player bits in discard/placement are mapped to (owner - view->player + 4)
 * % 4. Other bits and physical tile IDs are preserved. The five supplied
 * public face-up indicator IDs are sorted, with unused 255 slots at the end.
 * wall and all non-location view fields except view->player are ignored.
 *
 * All pointers must be non-NULL and arrays must have the declared capacity.
 * Indicators must be unique IDs in 0..135 or 255; candidate must be in 0..135.
 * Location fields must use cjong4 encodings; only the observer's hand may be
 * present. Returns false on invalid input, leaving output unchanged.
 *
 * The caller supplies public indicators and an offered legal discard. This
 * function does not verify legality, indicator visibility, or full state
 * consistency. Use cj4m_make_player_view(), never a copied full state.
 * No allocation occurs. The source view and indicators are not modified.
 */
bool cj4dr_encode_input(const cj4_player_view *view,
                       const cj4_tile_id dora_indicator_tile_ids[CJ4DR_DORA_COUNT],
                       cj4_tile_id candidate_tile_id,
                       uint8_t out_input[CJ4DR_INPUT_SIZE]);

/**
 * Check the exact byte length and canonical schema-v1 field encodings.
 * Hand owners must be 0; indicators must be unique, sorted, and padded by 255.
 * Does not verify discard legality or reconstruct/validate a game state.
 * Does not read input when NULL or size differs from CJ4DR_INPUT_SIZE.
 */
bool cj4dr_validate_input(const uint8_t *input, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* CJ4DR_INPUT_H */
