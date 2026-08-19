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
/* Per-(zone, layer) round-robin pool. The Plucked Acoustics banjo carries
   ELEVEN takes on one note, and the loader SKIPS files past this cap --
   at the old cap of 4 that was a silent drop of seven recordings that
   loaded in no error path anywhere. Sized past the deepest shipped pool;
   the cost is shorts in the index tables, so headroom is nearly free. */
#define AE_SMP_MAX_RR    12

/* Below this velocity a zone's SOFT pool is preferred where one exists.
   The soft files are genuinely softer-PLAYED recordings, peak-normalised
   to the main layer: the swap changes attack and spectrum, never level.
   Loudness is always the velocity gain -- that is what makes a quiet note
   sound played-quietly instead of turned-down. */
#define AE_SMP_SOFT_VEL  0.6

/* `loop_start`/`loop_end` are frame indices into `pcm`, both 0 when this
   recording has no usable sustain loop. Sustained voices -- bowed strings,
   winds, brass, organs, choir -- are one-shots that simply run out after
   6-8 seconds, so a held ghost died with the file rather than with its
   envelope. Looping the STEADY STATE is what lets the envelope decide.

   The points are DETECTED, never inherited: no source library in the set
   carries a `smpl` chunk, and the one format that could (the Mellotron SFZ
   set) declares `loop_mode=no_loop` on all 805 of its regions. They arrive
   as seconds from the recording's onset and are resolved to frames at load,
   where the seam crossfade is also baked into `pcm` -- so the audio thread
   only ever has to wrap an index. */
typedef struct
{
    float *pcm;
    int    len;
    int    midi; /* the pitch this file was RECORDED at */
    bool   soft;
    int    loop_start;
    int    loop_end;
} AeSampleRec;

/* Filename pitch vs SOUNDING pitch. Two shipped sets are named an octave
   off what they actually sound: bass filenames sit an octave ABOVE the
   sounding note, harpsichord an octave BELOW. Every other set is named at
   true pitch. The zone map and the rate math are self-consistent either
   way -- but a ghost asked for E4 has to SOUND E4, so the offset is folded
   in as files are indexed and everything downstream is in sounding pitch.
   `octave_override` (from sampleOctave) covers any set not in this table. */
typedef struct { const char *name; int semitones; } AeSampleOctave;

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
    /* Per-BANK level normalisation, measured at load. The shipped sets are
       peak-normalised to targets spanning ~16 dB, so the balance between
       instruments lives in the FILE MASTERING, not in any runtime
       constant -- switching instruments without this drops or lifts the
       ghosts by up to that much. Measured over the main layer only and
       applied to the whole bank, which is what preserves the soft layer's
       deliberate peak-match to it (a timbre swap, never a level change). */
    double norm;
    double meas_rms;  /* what was measured, for the read-out */
    int    clipped;   /* recordings that reach full scale. A properly
                         mastered set peaks BELOW 0 dBFS, so a file sitting
                         exactly at it is the signature of a decode gone
                         wrong -- the shipped pizzicato set once had 32
                         files decoded 24-bit-as-16-bit, which loaded and
                         played happily as full-scale noise. Silent
                         corruption deserves a number, not a shrug. */
    int    octave;    /* filename -> sounding offset actually applied */
} AeSampleBank;

/* Load one instrument from the <root>/<instrument> directory (control thread;
   allocates). A manifest may name a JSON file to take the file list from --
   any JSON is accepted, the engine simply harvests the ".wav" strings out
   of it, because the filename already carries instrument, zone, layer and
   variant. NULL/"" scans the directory instead. Returns false with a
   reason in err. */
/* `octave_override` shifts filename pitch to sounding pitch in SEMITONES;
   AE_SMP_OCTAVE_AUTO uses the built-in table for the known sets and 0
   otherwise. */
#define AE_SMP_OCTAVE_AUTO 99

bool ae_sampler_load (AeSampleBank *bank, const char *root,
                      const char *instrument, const char *manifest,
                      double engine_rate, int octave_override,
                      char *err, size_t err_len);

void ae_sampler_free (AeSampleBank *bank);

/* Enumerate the instruments present under a cache root: every subdirectory
   holding at least one file the zone parser recognises. Fills `names` and
   returns how many were written (sorted). The engine never carries a
   hard-coded instrument list -- adding one is dropping a folder in the
   cache, which is the whole operation, forever. */
/* The factory nine plus Acoustic Instruments' 24 plus a private set is
   already 34 -- and discovery TRUNCATES past the cap, which in a UI built
   from the echo means instruments that exist but cannot be picked. Sized
   with the same philosophy as the RR cap: far past what ships.

   Raised from 64 when the horn stabs took the shipped library to 61 dirs:
   three slots of headroom is not headroom, and the failure mode here is
   silent. The cost is a names table, so there is no reason to sit close.

   Raised again from 128 when the drum import took it to 138 -- the factory
   GM kit plus 46 percussion kits, each its own folder. That overran the cap
   outright, and the truncation is worse than a shortfall: the loop above
   stops at `max` and only sorts AFTERWARDS, so the instruments that vanish
   are whatever readdir happened to yield last. Nondeterministic, silent,
   and different on each machine. 256 is 118 past a library that grew by 47
   folders in one import. */
#define AE_SMP_MAX_INSTRUMENTS 256
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
