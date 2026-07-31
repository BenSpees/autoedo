/* IR file loading + hash verification for the convolution points. Treebrain
   is the librarian (owns the files and the manifest); this side loads a
   {path, hash} pair, verifies it, and hands the samples to the corrector's
   point under its crossfade. See ir_load.c for the hash convention. */

#ifndef AUTOEDO_IR_LOAD_H
#define AUTOEDO_IR_LOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "corrector.h"

/* FNV-1a 64-bit -- the shared content-hash convention (seed 0 = standard
   offset basis). */
uint64_t ae_ir_hash_bytes (const void *data, size_t len, uint64_t seed);

/* Load a WAV (PCM 16/24 or float32, mono/stereo, at the ENGINE rate) into
   a point: 0 = lead (stereo folds to mid), 1 = harmony (stereo L/R
   convolved independently). An empty/NULL path clears the point. hash ""
   skips verification (tests); otherwise it must match the file bytes.
   Control thread; returns false with a reason in err. */
bool ae_ir_load_point (AeCorrector *corr, int point, const char *path,
                       const char *hash, double predelay_ms, double engine_rate,
                       char *err, size_t err_len);

#endif /* AUTOEDO_IR_LOAD_H */
