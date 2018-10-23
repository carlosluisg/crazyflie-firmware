/**
 Exposes the public API used for clock correction in an Object Oriented way (and hides the functions that are only part of the internal implementation).
 */

#ifndef clockCorrectionEngine_h
#define clockCorrectionEngine_h

#include <stdbool.h>
#include "clockCorrectionStorage.h"

typedef struct {
  double (*getClockCorrection)(const clockCorrectionStorage_t* storage);
  double (*calculateClockCorrection)(const uint64_t new_t_in_cl_reference, const uint64_t old_t_in_cl_reference, const uint64_t new_t_in_cl_x, const uint64_t old_t_in_cl_x, const uint64_t mask);
  bool (*updateClockCorrection)(clockCorrectionStorage_t* storage, const double clockCorrectionCandidate);

} clockCorrectionEngine_t;

extern clockCorrectionEngine_t clockCorrectionEngine;

#endif /* clockCorrectionEngine_h */
