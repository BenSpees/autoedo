/* C ABI wrapper around Signalsmith Stretch (third_party/signalsmith-stretch),
   the phase-vocoder pitch shifter that replaced the in-house TD-PSOLA
   resynthesis. One instance = one independently-shifted voice.

   Everything that allocates happens in ae_shifter_create(); process() is
   allocation-free and safe on the audio thread. */

#ifndef AUTOEDO_SHIFTER_H
#define AUTOEDO_SHIFTER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeShifter AeShifter;

/* Quality presets: block size sets both the spectral resolution and the
   latency (latency == block). Measured on a 220 Hz harmonic tone:
   small corrections (< 1 semitone) stay within a few cents at every
   setting, but large harmony intervals need the bigger blocks to land
   accurately (+7 semitones: ~30 cents off at LOW, ~7 at BALANCED,
   < 1 at HIGH). */
typedef enum
{
    AE_SHIFT_QUALITY_LOW      = 0, /* 25 ms block — lowest latency */
    AE_SHIFT_QUALITY_BALANCED = 1, /* 45 ms block — default */
    AE_SHIFT_QUALITY_HIGH     = 2  /* 120 ms block — the library's own preset */
} AeShifterQuality;

/* Block size (samples) for a quality preset at this sample rate. */
int ae_shifter_block_samples (double sample_rate, int quality);

/* Create a mono shifter. block_samples <= 0 uses the BALANCED preset.
   Returns NULL on failure. */
AeShifter *ae_shifter_create (double sample_rate, int block_samples);
void       ae_shifter_destroy (AeShifter *s);

/* Clear internal state (use when a voice has been silent and is about to
   sound again, so it can't replay stale audio). */
void ae_shifter_reset (AeShifter *s);

/* Set the pitch shift. `tonality_limit_hz` > 0 keeps frequencies above it
   closer to their original values, which preserves some timbre on large
   shifts; pass 0 for a plain linear shift. */
void ae_shifter_set_semitones (AeShifter *s, double semitones,
                               double tonality_limit_hz);

/* Shift formants by `semitones` (compensate = true also undoes the pitch
   shift's own formant movement). No-op unless the vendored library
   supports formants — see ae_shifter_has_formant_support(). */
void ae_shifter_set_formant_semitones (AeShifter *s, double semitones,
                                       bool compensate);

/* True when the vendored Signalsmith Stretch exposes formant control
   (v1.3+). The v1.1 series bundled here does not; the call above is then
   a no-op and large harmony intervals move formants with the pitch. */
bool ae_shifter_has_formant_support (void);

/* Total latency in samples (input + output halves): output sample i
   corresponds to input sample i - latency. Constant after create(). */
int ae_shifter_latency (const AeShifter *s);

/* Process n samples. `in` and `out` must not alias. Allocation-free. */
void ae_shifter_process (AeShifter *s, const float *in, float *out, int n);

/* "major.minor.patch" of the vendored library, for the status read-out. */
const char *ae_shifter_version (void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AUTOEDO_SHIFTER_H */
