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

/* RGB bytes enter the pixel layers unchanged (0..255).
 * Every layer: INT32 weighted sum + bias, then max(0,sum) / 2^shift (floor),
 * then saturation. Pixel/hidden layers saturate at 127, output at 255.
 * Candidate weights are added to the hidden sum before its shift.
 * Validated bias bounds guarantee no signed overflow for any INT8 weights.
 *
 * Requires explicit model weights and non-overlapping input/model/workspace/
 * output storage. Checks model and input on each call. No dynamic allocation,
 * global mutable state, floating point, or libm. On failure neither workspace
 * nor out_danger is changed. A successful score is not a calibrated probability
 * or a guarantee of safety. Quality depends on supplied trained weights. */
bool cj4dr_evaluate(const cj4dr_model *model, const uint8_t *input, size_t size,
                   cj4dr_workspace *workspace, uint8_t *out_danger);

#ifdef __cplusplus
}
#endif
#endif /* CJ4DR_EVALUATOR_H */
