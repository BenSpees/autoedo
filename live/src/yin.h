/* Monophonic fundamental-frequency estimator using the YIN algorithm
   (de Cheveigné & Kawahara, 2002). C port of Source/dsp/YinPitchDetector. */

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
    double threshold; /* CMND acceptance threshold (~0.1-0.15) */

    double *diff;       /* difference function d(tau) */
    double *cumulative; /* cumulative mean normalised difference d'(tau) */
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
