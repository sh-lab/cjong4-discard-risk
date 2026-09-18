#ifndef CJ4DR_TEACHER_H
#define CJ4DR_TEACHER_H

#include "cjong4/manager/manager.h"
#include "cjong4_discard_risk/input.h"

/* Training-only helpers. Full state never enters the inference API. */
typedef struct {
    uint8_t rgb[CJ4DR_INPUT_SIZE];
    uint8_t target[CJ4_TILE_TYPE_COUNT];
    uint8_t mask[CJ4_TILE_TYPE_COUNT];
} cj4dr_teacher;

typedef enum {
    CJ4DR_CASE_ALL_GENBUTSU,
    CJ4DR_CASE_RIICHI_PASSED,
    CJ4DR_CASE_RIICHI_DANGER,
    CJ4DR_CASE_COUNT
} cj4dr_case;

uint64_t cj4dr_random(uint64_t *rng);
uint32_t cj4dr_uniform(uint64_t *rng, uint32_t bound);
void cj4dr_shuffle(cj4_tile_id wall[136], uint64_t *rng);

/* Label ordinary legal discards only: any opponent can ron => 255, else 0.
 * Unheld/illegal types remain unspecified. Failure preserves output. */
bool cj4dr_make_teacher(const cj4_mahjong *state, const cj4_rules *rules,
                       cj4dr_teacher *output);

/* Build a complete legal wall, then replay discards/riichi/passes. */
bool cj4dr_make_case(cj4dr_case kind, cj4_tile_type type, uint64_t seed,
                    const cj4_rules *rules, cj4_mahjong *output);
const char *cj4dr_case_name(cj4dr_case kind);

#endif
