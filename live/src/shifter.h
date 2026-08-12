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

/* Quality presets. The analysis block sets the spectral resolution, and the
   latency is that block plus one interval (a quarter block) for the split
   computation the shifters run in. Measured end to end across 110-440 Hz:
   correction lands within ~10 cents at LOW/BALANCED and ~5 at HIGH, while
   large harmony intervals need HIGH — below it they drift sharp on low
   voices, and LOW can lose the octave entirely on a bass. */
typedef enum
{
    AE_SHIFT_QUALITY_LOW      = 0, /* 25 ms block -> 31 ms latency */
    AE_SHIFT_QUALITY_BALANCED = 1, /* 45 ms block -> 56 ms latency (default) */
    AE_SHIFT_QUALITY_HIGH     = 2  /* 120 ms block -> 150 ms (library preset) */
    /* OR this into ae_corrector_prepare's quality argument for POLY mode:
       detection is bypassed and the shifters run at fixed ratios over the
       whole polyphonic input, with the analysis block DOUBLED -- chords
       need the longer window, and the extra latency is poly mode's
       documented price. Kept as a flag so the two-axis setting rides one
       argument without touching every prepare call site. */

} AeShifterQuality;

#define AE_SHIFT_QUALITY_POLY_FLAG 0x100

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
   shift's own formant movement, which is what keeps a transposed voice
   sounding like the same singer). No-op unless the vendored library
   supports formants — see ae_shifter_has_formant_support(). */
void ae_shifter_set_formant_semitones (AeShifter *s, double semitones,
                                       bool compensate);

/* Tell the formant analysis roughly where the fundamental sits (Hz); the
   library asks for this to separate formants from the harmonic series. We
   have it from YIN, so we pass the detected pitch. 0 = let it guess.
   No-op without formant support. */
void ae_shifter_set_formant_base (AeShifter *s, double base_hz);

/* True when the vendored Signalsmith Stretch exposes formant control
   (v1.3+). Without it the two calls above are no-ops and large harmony
   intervals move formants with the pitch. */
bool ae_shifter_has_formant_support (void);

/* Total latency in samples (input + output halves, including the extra
   interval that split computation costs): output sample i corresponds to
   input sample i - latency. Constant after create(). */
int ae_shifter_latency (const AeShifter *s);

/* The level-match's current makeup gain, LINEAR (1 = doing nothing). A
   diagnostic read-out: persistent values far from 1 mean the level match
   is working hard, which on a lead is worth knowing before blaming tone. */
float ae_shifter_makeup (const AeShifter *s);

/* Process n samples. `in` and `out` must not alias. Allocation-free. */
void ae_shifter_process (AeShifter *s, const float *in, float *out, int n);

/* "major.minor.patch" of the vendored library, for the status read-out. */
const char *ae_shifter_version (void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AUTOEDO_SHIFTER_H */
