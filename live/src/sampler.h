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
/* 128 -- one slot per MIDI note, so no instrument can outgrow it again. The
   four General MIDI sets are sampled at every semitone from A0 to C8, which
   is 88 zones and would have overflowed the old limit of 64: past it
   zone_slot returns -1 and the recording is dropped at load, so an
   instrument would simply have stopped halfway up its range. Sized to the
   note space rather than to today's widest set. */
#define AE_SMP_MAX_ZONES 128
/* 8. The first nine sets never went past 3 main / 2 soft, but the
   plucked-acoustics banjo carries up to ELEVEN takes of one note, and a
   variant past this limit is dropped at load without a word -- so the limit
   has to be a decision rather than an accident. Eight is the ceiling we
   allow.

   It costs three files. Exactly one note in the pack is above eight (banjo
   As3, at eleven); every other banjo note has seven or fewer and no other
   instrument has more than five. That note still round-robins over eight
   distinct takes, which is far past the point where a repeat is audible --
   the failure this exists to prevent is REUSING the recording you just
   played, and eight makes that as rare as twelve would.

   WHICH eight survive is readdir order, i.e. arbitrary. That is fine: they
   are interchangeable takes of the same note at the same dynamic, so there
   is no "right" eight to prefer. Treebrain's librarian has no such limit and
   serves all eleven; the two engines differing on one note of one instrument
   is not worth a fixed-size array here. */
#define AE_SMP_MAX_RR    8

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
#define AE_SMP_MAX_INSTRUMENTS 32
int ae_sampler_list (const char *root, char names[][32], int max);

/* The nearest zone by PITCH to `midi`, or -1 for an empty bank. */
int ae_sampler_zone (const AeSampleBank *bank, int midi);

/* Pick a recording from a zone: the soft pool below AE_SMP_SOFT_VEL where
   one exists, then a round-robin variant that is never the immediately
   previous pick for that (zone, layer) -- a repeated note that reuses its
   recording machine-guns at once. `rr_state` is the caller's last-pick
   memory, indexed zone*2+soft: AE_SMP_MAX_ZONES * 2 entries, init to -1.
   (Two per zone, not one -- the main and soft pools round-robin
   independently.) */
const AeSampleRec *ae_sampler_pick (const AeSampleBank *bank, int zone,
                                    double velocity, signed char *rr_state,
                                    unsigned *rng);

#endif /* AUTOEDO_SAMPLER_H */
