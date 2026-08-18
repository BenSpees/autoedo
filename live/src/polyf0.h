/* Real-time polyphonic pitch tracking for POLY sample mode.

   Multi-f0 by harmonic summation with iterative spectral subtraction --
   the practical real-time algorithm for this job: score every candidate
   fundamental by how much of the spectrum its harmonic comb explains,
   take the best, subtract its comb, repeat up to the polyphony cap. On
   top of the per-frame estimates sits a note tracker with birth/death
   hysteresis, because a chord sampler must strike NOTES, not frames: one
   glitchy frame must neither strike a note nor kill one.

   Honest scope: harmonic subtraction handles the chords a guitarist
   actually strums (2-5 distinct pitch classes) well, and octave
   doublings or very dense voicings imperfectly. The EHX 9-series pedals
   avoid this problem by never detecting notes at all (they reshape the
   input's own spectrum) -- the price of striking a real sample library
   per note is that detection can miss; the prize is that the library can
   be anything, at any tuning. */

#ifndef AUTOEDO_POLYF0_H
#define AUTOEDO_POLYF0_H

#include <stdbool.h>

#define AE_POLY_MAX_NOTES 6

typedef struct
{
    double hz;      /* tracked fundamental (frame-smoothed) */
    double level;   /* salience-derived level, 0..1-ish (relative) */
    double raw;     /* UN-normalised salience: absolute-ish energy, so a
                       consumer can see one note's own level jump between
                       frames (a re-pluck) where the relative `level`
                       cannot -- a new loud note renormalises everyone */
    int    id;      /* stable across the note's life; slots reusable */
    bool   active;
} AePolyNote;

typedef struct
{
    double fs;
    int    win_size;    /* analysis WINDOW: the samples the caller feeds */
    int    fft_size;    /* zero-padded transform = 2 * win_size */
    int    max_notes;
    double min_hz, max_hz;

    /* Scratch (allocated in prepare) */
    double *re, *im;    /* FFT work */
    double *mag;        /* residual spectrum under iterative subtraction */
    double *mag0;       /* pristine spectrum (refinement reads this) */
    float  *win;        /* Hann window */

    /* The frame's energy gate, as an input RMS (amplitude, not power).
       Frames whose windowed power falls below ~2x this yield no
       candidates -- silence must not birth noise-floor ghosts. The
       CALLER owns the value (the corrector mirrors its own noise gate
       here): an absolute gate was the field failure "randomly produces
       no output" -- it sat ~20 dB above the engine's own gate, so quiet
       playing was voiced to every other subsystem and invisible to this
       one. Prepare defaults it to the engine's default gate. */
    double gate_rms;

    /* Births normally need two consecutive sightings (one glitchy frame
       is not a note). While the CALLER knows a physical attack just
       happened (its onset detector fired), one sighting is enough: the
       transient is the corroborating witness, and waiting a second frame
       is pure latency on the commonest event there is. The caller sets
       this while its onset burst runs. */
    bool   fast_birth;

    /* Tracker state */
    AePolyNote notes[AE_POLY_MAX_NOTES];
    int    miss[AE_POLY_MAX_NOTES];  /* consecutive frames unseen */
    int    seen[AE_POLY_MAX_NOTES];  /* consecutive frames seen (pre-birth) */
    double cand_hz[AE_POLY_MAX_NOTES];
    double cand_lv[AE_POLY_MAX_NOTES];
    int    next_id;
} AePolyF0;

/* Prepare for a rate and range (allocates). fft_size is chosen from the
   range: two periods of min_hz, next power of two, floor 2048. */
void ae_polyf0_prepare (AePolyF0 *t, double fs, double min_hz, double max_hz);
void ae_polyf0_free (AePolyF0 *t);

/* Analyse one frame of win_size samples (most recent audio). Updates the
   tracker; read results from t->notes[]. Allocation-free. */
void ae_polyf0_process (AePolyF0 *t, const float *frame);

#endif /* AUTOEDO_POLYF0_H */
