#ifndef CJ4DR_EVALUATOR_H
#define CJ4DR_EVALUATOR_H

#include "cjong4_discard_risk/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 552 bytes, reusable by successive calls; use a separate workspace per thread. */
typedef struct {
    uint8_t pixels[CJ4DR_CONTEXT_SIZE];
    uint8_t hidden[CJ4DR_HIDDEN_SIZE];
} cj4dr_workspace;

/* Score all 34 tile types in one call, including types not in the player's hand.
 * out_danger must hold 34 bytes: 1m..9m, 1p..9p, 1s..9s, E/S/W/N, white/green/red.
 * Each score is independent, 0x00 (lowest danger) .. 0xff (highest).
 * The caller filters legal discards and uses out_danger[cj4_tile_get_type(id)].
 *
 * RGB bytes enter the pixel layers unchanged (0..255).
 * Every layer: INT32 weighted sum + bias, then max(0,sum) / 2^shift (floor),
 * then saturation. Pixel/hidden layers saturate at 127, outputs at 255.
 * Validated bias bounds guarantee no signed overflow for any INT8 weights.
 *
 * Requires explicit weights and non-overlapping input/model/workspace/output.
 * Checks model and input on each call. No allocation, global mutable state,
 * floating point, or libm. On failure the workspace and all 34 output bytes
 * remain unchanged. Scores are not calibrated probabilities or safety
 * guarantees; even examples trained with target 0 need not produce exactly 0. */
bool cj4dr_evaluate(const cj4dr_model *model, const uint8_t *input, size_t size,
                   cj4dr_workspace *workspace,
                   uint8_t out_danger[CJ4DR_OUTPUT_SIZE]);

#ifdef __cplusplus
}
#endif
#endif /* CJ4DR_EVALUATOR_H */
