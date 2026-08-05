/* Monophonic fundamental-frequency estimator using the YIN algorithm
   (de Cheveigné & Kawahara, 2002). C port of Source/dsp/YinPitchDetector.

   The squared-difference function is evaluated through an FFT
   cross-correlation rather than the textbook double loop: same answer to
   within floating-point noise, but O(N log N) instead of O(tau_max * window).
   At 96 kHz that is ~3.9 M multiply-adds per analysis frame down to ~0.2 M,
   every 5 ms -- headroom this engine spends on six pitch shifters, LPC and
   convolution. */

#ifndef AUTOEDO_YIN_H
#define AUTOEDO_YIN_H

#include <stdbool.h>

typedef struct
{
    double frequency_hz; /* estimated f0 (0 if no pitch found) */
    double periodicity;  /* confidence in [0, 1] (1 = perfectly periodic) */
    bool   voiced;
} AeYinResult;

typedef struct
{
    double sample_rate;
    int    frame_size;
    int    tau_min;
    int    tau_max;
    int    window;    /* integration window length */
    int    fft_size;  /* >= window + tau_max, power of two */
    double threshold; /* CMND acceptance threshold (~0.1-0.15) */

    int     last_best_tau; /* previous voiced frame's lag; 0 = none. Lets
                              the octave guard demand a clearly better dip
                              before CHANGING octave between frames. */
    double *diff;       /* difference function d(tau) */
    double *cumulative; /* cumulative mean normalised difference d'(tau) */

    /* Scratch for the FFT cross-correlation that evaluates d(tau). Real and
       imaginary parts are kept in separate arrays rather than C99 _Complex,
       which MSVC does not support for C. */
    double *a_re, *a_im, *b_re, *b_im;
} AeYin;

/* Prepare for a sample rate and analysis frame size (allocates). Safe to call
   again to re-prepare. */
void ae_yin_prepare (AeYin *y, double sample_rate, int frame_size,
                     double min_frequency, double max_frequency);

/* Release buffers allocated by ae_yin_prepare(). */
void ae_yin_free (AeYin *y);

/* Analyse one frame of frame_size samples. Allocation-free. */
AeYinResult ae_yin_process (AeYin *y, const float *frame, int num_samples);

#endif /* AUTOEDO_YIN_H */
