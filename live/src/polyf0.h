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
