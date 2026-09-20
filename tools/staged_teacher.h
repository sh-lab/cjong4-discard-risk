#ifndef CJ4DR_STAGED_TEACHER_H
#define CJ4DR_STAGED_TEACHER_H

#include "teacher.h"

/* Public passed-tile evidence only. Initialize to zero at the start of a round
 * and observe every manager transition. Never supplied to the NN. */
typedef struct {
    uint8_t passed[CJ4_PLAYER_COUNT][CJ4_TILE_TYPE_COUNT];
} cj4dr_safety;

void cj4dr_safety_observe(cj4dr_safety *safety, const cj4_mahjong *before,
                         const cj4_mahjong *after);

/* Valid view/type and chronological safety tracker required. Open melds exclude
 * ankan; a kakan counts as one meld. No hidden hand/ron query is used. */
uint8_t cj4dr_stage_score(const cj4_player_view *view, const cj4dr_safety *safety,
                         cj4_tile_type type);
bool cj4dr_make_staged_teacher(const cj4_mahjong *state, const cj4_rules *rules,
                              const cj4dr_safety *safety, cj4dr_teacher *output);

/* Call only AFTER an actual ordinary-discard ron; RGB remains pre-discard. */
bool cj4dr_mark_actual_ron(cj4dr_teacher *sample, int discarded);

#endif
