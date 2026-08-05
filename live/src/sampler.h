/* Sample bank for the Xentar pitched library.

   Treebrain is the librarian: it transcodes the shipped MP3s once into
   mono float32 WAV at the engine's rate and hands over a cache root. This
   side never decodes an MP3 and never resamples -- it reads the WAVs of
   ONE instrument, indexes them by recording pitch, and serves a
   (zone, layer, variant) pick to the audio thread.

   Everything a recording needs to be identified is in its filename --
   <Note><Octave>[_soft][_rrN].wav -- so a manifest is a file LIST, not a
   schema the engine has to agree with. */

#ifndef AUTOEDO_SAMPLER_H
#define AUTOEDO_SAMPLER_H

#include <stdbool.h>
#include <stddef.h>

#define AE_SMP_MAX_RECS  320  /* the largest shipped instrument is 120 */
#define AE_SMP_MAX_ZONES 64
#define AE_SMP_MAX_RR    4    /* shipped max is 3 main / 2 soft */

/* Below this velocity a zone's SOFT pool is preferred where one exists.
   The soft files are genuinely softer-PLAYED recordings, peak-normalised
   to the main layer: the swap changes attack and spectrum, never level.
   Loudness is always the velocity gain -- that is what makes a quiet note
   sound played-quietly instead of turned-down. */
#define AE_SMP_SOFT_VEL  0.6

typedef struct
{
    float *pcm;
    int    len;
    int    midi; /* the pitch this file was RECORDED at */
    bool   soft;
} AeSampleRec;

typedef struct
{
    AeSampleRec recs[AE_SMP_MAX_RECS];
    int         n_recs;

    int  zones[AE_SMP_MAX_ZONES]; /* sorted, distinct recording pitches */
    int  n_zones;
    short main_idx[AE_SMP_MAX_ZONES][AE_SMP_MAX_RR];
    short soft_idx[AE_SMP_MAX_ZONES][AE_SMP_MAX_RR];
    int   main_n[AE_SMP_MAX_ZONES];
    int   soft_n[AE_SMP_MAX_ZONES];

    char   instrument[32];
    double rate;      /* the rate the files were cached at */
    size_t bytes;     /* total PCM footprint, for the status read-out */
} AeSampleBank;

/* Load one instrument from the <root>/<instrument> directory (control thread;
   allocates). A manifest may name a JSON file to take the file list from --
   any JSON is accepted, the engine simply harvests the ".wav" strings out
   of it, because the filename already carries instrument, zone, layer and
   variant. NULL/"" scans the directory instead. Returns false with a
   reason in err. */
bool ae_sampler_load (AeSampleBank *bank, const char *root,
                      const char *instrument, const char *manifest,
                      double engine_rate, char *err, size_t err_len);

void ae_sampler_free (AeSampleBank *bank);

/* Enumerate the instruments present under a cache root: every subdirectory
   holding at least one file the zone parser recognises. Fills `names` and
   returns how many were written (sorted). The engine never carries a
   hard-coded instrument list -- adding one is dropping a folder in the
   cache, which is the whole operation, forever. */
#define AE_SMP_MAX_INSTRUMENTS 32
int ae_sampler_list (const char *root, char names[][32], int max);

/* The nearest zone by PITCH to `midi`, or -1 for an empty bank. */
int ae_sampler_zone (const AeSampleBank *bank, int midi);

/* Pick a recording from a zone: the soft pool below AE_SMP_SOFT_VEL where
   one exists, then a round-robin variant that is never the immediately
   previous pick for that (zone, layer) -- a repeated note that reuses its
   recording machine-guns at once. `rr_state` is the caller's per-zone
   last-pick memory (AE_SMP_MAX_ZONES entries, init to -1). */
const AeSampleRec *ae_sampler_pick (const AeSampleBank *bank, int zone,
                                    double velocity, signed char *rr_state,
                                    unsigned *rng);

#endif /* AUTOEDO_SAMPLER_H */
