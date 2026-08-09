#include "sampler.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  void ae_engine_sleep_ms (int ms) { Sleep ((DWORD) ms); }
#else
  #include <unistd.h>
  void ae_engine_sleep_ms (int ms) { usleep ((useconds_t) ms * 1000); }
#endif

/* ---------------------------------------------------------------- WAV read */

static unsigned rd_u32 (const unsigned char *p)
{
    return (unsigned) p[0] | ((unsigned) p[1] << 8)
         | ((unsigned) p[2] << 16) | ((unsigned) p[3] << 24);
}
static unsigned rd_u16 (const unsigned char *p)
{
    return (unsigned) p[0] | ((unsigned) p[1] << 8);
}

/* Read a mono/stereo WAV (float32 or PCM16/24) into malloc'd mono floats.
   Treebrain guarantees mono float32 at the engine rate; the other cases are
   handled defensively so a hand-made cache still plays rather than
   silently producing nothing. Stereo folds to mid. */
static float *wav_read_file (const char *path, int *out_frames, double *out_rate)
{
    FILE *f = fopen (path, "rb");
    if (f == NULL)
        return NULL;
    fseek (f, 0, SEEK_END);
    long fl = ftell (f);
    fseek (f, 0, SEEK_SET);
    if (fl < 44 || fl > (long) (256 << 20)) { fclose (f); return NULL; }
    unsigned char *b = malloc ((size_t) fl);
    if (b == NULL) { fclose (f); return NULL; }
    const size_t got = fread (b, 1, (size_t) fl, f);
    fclose (f);
    if (got != (size_t) fl
        || memcmp (b, "RIFF", 4) != 0 || memcmp (b + 8, "WAVE", 4) != 0)
    { free (b); return NULL; }

    int ch = 0, bits = 0, fmt = 0;
    double rate = 0.0;
    size_t off = 12, dpos = 0, dlen = 0;
    while (off + 8 <= (size_t) fl)
    {
        const unsigned sz = rd_u32 (b + off + 4);
        if (memcmp (b + off, "fmt ", 4) == 0 && sz >= 16)
        {
            fmt  = (int) rd_u16 (b + off + 8);
            ch   = (int) rd_u16 (b + off + 10);
            rate = (double) rd_u32 (b + off + 12);
            bits = (int) rd_u16 (b + off + 22);
        }
        else if (memcmp (b + off, "data", 4) == 0)
        {
            dpos = off + 8;
            dlen = sz;
            if (dpos + dlen > (size_t) fl) dlen = (size_t) fl - dpos;
        }
        off += 8 + sz + (sz & 1u);
    }
    if (ch < 1 || ch > 8 || dlen == 0
        || ! ((fmt == 3 && bits == 32) || (fmt == 1 && (bits == 16 || bits == 24))))
    { free (b); return NULL; }

    const int bytes = bits / 8;
    const int frames = (int) (dlen / (size_t) (bytes * ch));
    float *out = calloc ((size_t) (frames > 0 ? frames : 1), sizeof (float));
    if (out == NULL) { free (b); return NULL; }
    for (int i = 0; i < frames; ++i)
    {
        double acc = 0.0;
        for (int c = 0; c < ch; ++c)
        {
            const unsigned char *s = b + dpos + (size_t) (i * ch + c) * bytes;
            if (fmt == 3)
            {
                float v; memcpy (&v, s, 4); acc += v;
            }
            else if (bits == 16)
                acc += (double) (short) rd_u16 (s) / 32768.0;
            else
            {
                int v = (int) ((unsigned) s[0] | ((unsigned) s[1] << 8)
                                               | ((unsigned) s[2] << 16));
                if (v & 0x800000) v -= 0x1000000;
                acc += (double) v / 8388608.0;
            }
        }
        out[i] = (float) (acc / ch);
    }
    free (b);
    *out_frames = frames;
    *out_rate = rate;
    return out;
}

/* ----------------------------------------------------------- filename parse */

/* "<Note><Octave>[_soft][_rrN].wav" -> midi + layer. Sharps appear as both
   '#' and 's' in the shipped set; both are accepted. Returns -1 if the
   name is not a zone file. */
static int parse_name (const char *name, bool *soft)
{
    static const int pc_of[7] = { 9, 11, 0, 2, 4, 5, 7 }; /* A B C D E F G */
    const char *p = name;
    if (*p < 'A' || *p > 'G')
        return -1;
    int pc = pc_of[*p - 'A'];
    ++p;
    if (*p == '#' || *p == 's') { pc = (pc + 1) % 12; ++p; }
    if (*p < '0' || *p > '9')
        return -1;
    int oct = 0;
    while (*p >= '0' && *p <= '9') { oct = oct * 10 + (*p - '0'); ++p; }
    if (oct > 10)
        return -1;

    *soft = false;
    if (strncmp (p, "_soft", 5) == 0) { *soft = true; p += 5; }
    if (strncmp (p, "_rr", 3) == 0)
    {
        p += 3;
        while (*p >= '0' && *p <= '9') ++p;
    }
    if (strcmp (p, ".wav") != 0)
        return -1;
    /* MIDI: C-1 = 0, so C<oct> = 12*(oct+1). */
    return 12 * (oct + 1) + pc;
}

/* The two shipped sets whose filenames are not their sounding pitch. */
static const AeSampleOctave k_octaves[] = {
    { "bass",        -12 }, /* named an octave ABOVE what it sounds */
    { "harpsichord",  12 }, /* named an octave BELOW */
};

static int octave_for (const char *instrument, int override_st)
{
    if (override_st != AE_SMP_OCTAVE_AUTO)
        return override_st;
    for (size_t i = 0; i < sizeof (k_octaves) / sizeof (k_octaves[0]); ++i)
        if (strcmp (instrument, k_octaves[i].name) == 0)
            return k_octaves[i].semitones;
    return 0;
}

static int zone_slot (AeSampleBank *b, int midi)
{
    for (int i = 0; i < b->n_zones; ++i)
        if (b->zones[i] == midi)
            return i;
    if (b->n_zones >= AE_SMP_MAX_ZONES)
        return -1;
    const int i = b->n_zones++;
    b->zones[i] = midi;
    b->main_n[i] = b->soft_n[i] = 0;
    return i;
}

static void add_file (AeSampleBank *b, const char *dir, const char *name,
                      double engine_rate)
{
    bool soft = false;
    int midi = parse_name (name, &soft);
    if (midi < 0 || b->n_recs >= AE_SMP_MAX_RECS)
        return;
    midi += b->octave; /* index in SOUNDING pitch, not filename pitch */
    const int z = zone_slot (b, midi);
    if (z < 0)
        return;
    int *n = soft ? &b->soft_n[z] : &b->main_n[z];
    if (*n >= AE_SMP_MAX_RR)
        return;

    char path[1200];
    snprintf (path, sizeof (path), "%s/%s", dir, name);
    int frames = 0;
    double rate = 0.0;
    float *pcm = wav_read_file (path, &frames, &rate);
    if (pcm == NULL || frames <= 0)
    {
        free (pcm);
        return; /* fail-soft: a bad file is a missing variant, never a stall */
    }
    /* A cache at the wrong rate would play at the wrong pitch AND the wrong
       speed; refuse it rather than transpose around it. 1% covers 48000 vs
       47999.6-style rounding in a WAV header. */
    if (engine_rate > 0.0 && rate > 0.0
        && (rate < engine_rate * 0.99 || rate > engine_rate * 1.01))
    {
        free (pcm);
        return;
    }

    /* Full-scale peak = a decode that went wrong (see AeSampleBank.clipped). */
    {
        int at_fs = 0;
        for (int i = 0; i < frames; ++i)
            if (pcm[i] >= 0.9999f || pcm[i] <= -0.9999f)
                if (++at_fs >= 8)
                    break;
        if (at_fs >= 8)
            b->clipped++;
    }

    const int idx = b->n_recs++;
    b->recs[idx].pcm  = pcm;
    b->recs[idx].len  = frames;
    b->recs[idx].midi = midi;
    b->recs[idx].soft = soft;
    b->bytes += (size_t) frames * sizeof (float);
    if (soft) b->soft_idx[z][(*n)++] = (short) idx;
    else      b->main_idx[z][(*n)++] = (short) idx;
}

/* Harvest every "....wav" string out of whatever JSON the librarian wrote.
   The manifest is a file LIST here, not a schema: the filename already
   carries instrument, zone, layer and variant, so there is nothing to
   agree about and a manifest shape change cannot break the engine. */
static int load_from_manifest (AeSampleBank *b, const char *manifest,
                               const char *dir, const char *instrument,
                               double engine_rate)
{
    FILE *f = fopen (manifest, "rb");
    if (f == NULL)
        return -1;
    fseek (f, 0, SEEK_END);
    long fl = ftell (f);
    fseek (f, 0, SEEK_SET);
    if (fl <= 0 || fl > (16 << 20)) { fclose (f); return -1; }
    char *txt = malloc ((size_t) fl + 1);
    if (txt == NULL) { fclose (f); return -1; }
    const size_t got = fread (txt, 1, (size_t) fl, f);
    fclose (f);
    txt[got] = '\0';

    int found = 0;
    for (const char *p = txt; (p = strstr (p, ".wav")) != NULL; p += 4)
    {
        /* Walk back to the start of this quoted string, then forward past
           any directory part -- the instrument folder is ours to supply. */
        const char *s = p;
        while (s > txt && s[-1] != '"' && s[-1] != '\n')
            --s;
        const char *base = p;
        while (base > s && base[-1] != '/' && base[-1] != '\\')
            --base;
        /* Only files of the instrument we were asked for. */
        if (base > s + 1)
        {
            const size_t dlen = (size_t) (base - s) - 1;
            const size_t ilen = strlen (instrument);
            if (dlen < ilen || strncmp (base - 1 - ilen, instrument, ilen) != 0)
                continue;
        }
        char name[256];
        const size_t nlen = (size_t) (p - base) + 4;
        if (nlen >= sizeof (name))
            continue;
        memcpy (name, base, nlen);
        name[nlen] = '\0';
        add_file (b, dir, name, engine_rate);
        ++found;
    }
    free (txt);
    return found;
}

/* The bank's own level, measured over the first 300 ms of each MAIN-layer
   recording -- the note's body, not its tail, which on a long decay would
   dominate an over-the-whole-file figure and read a piano as quiet. The
   median is taken rather than the mean so one hot or dead zone cannot move
   the instrument. */
static void measure_bank (AeSampleBank *b)
{
    b->norm = 1.0;
    b->meas_rms = 0.0;
    if (b->n_recs == 0)
        return;

    double rms[AE_SMP_MAX_RECS];
    int n = 0;
    const int win = (int) (0.300 * (b->rate > 0.0 ? b->rate : 48000.0));
    for (int i = 0; i < b->n_recs; ++i)
    {
        if (b->recs[i].soft)
            continue; /* the soft layer rides its mastered relationship */
        const int len = b->recs[i].len < win ? b->recs[i].len : win;
        if (len <= 0)
            continue;
        double sq = 0.0;
        for (int k = 0; k < len; ++k)
            sq += (double) b->recs[i].pcm[k] * b->recs[i].pcm[k];
        rms[n++] = sqrt (sq / len);
    }
    if (n == 0)
        return;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && rms[j] < rms[j - 1]; --j)
        {
            const double t = rms[j]; rms[j] = rms[j - 1]; rms[j - 1] = t;
        }
    b->meas_rms = rms[n / 2];
    /* Reference: -22 dBFS, the median of the shipped sets' own levels, so
       an instrument at the middle of the library is left alone and the
       outliers are brought to it. Clamped so a pathological bank cannot
       turn into a boost. */
    if (b->meas_rms > 1e-6)
    {
        b->norm = 0.0794328 / b->meas_rms; /* 10^(-22/20) */
        if (b->norm < 0.1)  b->norm = 0.1;
        if (b->norm > 10.0) b->norm = 10.0;

        /* Peak ceiling: normalisation must never push a recording's peak
           into the clip. The 300 ms window reads a slow swell as near
           silence -- Acoustic Instruments' bowed vibraphone measured a
           +20 dB "correction" whose peaks would have landed +14 dBFS --
           so the boost is capped where the bank's loudest sample would
           reach -3 dBFS. A swell IS quiet early; that is the instrument,
           not a mastering fault to correct. */
        double peak = 0.0;
        for (int i = 0; i < b->n_recs; ++i)
            for (int j = 0; j < b->recs[i].len; ++j)
            {
                const double a = fabs ((double) b->recs[i].pcm[j]);
                if (a > peak) peak = a;
            }
        if (peak > 1e-6 && b->norm * peak > 0.708)
            b->norm = 0.708 / peak; /* -3 dBFS ceiling */
    }
}

bool ae_sampler_load (AeSampleBank *bank, const char *root,
                      const char *instrument, const char *manifest,
                      double engine_rate, int octave_override,
                      char *err, size_t err_len)
{
    ae_sampler_free (bank);
    if (root == NULL || root[0] == '\0' || instrument == NULL || instrument[0] == '\0')
    {
        snprintf (err, err_len, "no sample path or instrument set");
        return false;
    }
    snprintf (bank->instrument, sizeof (bank->instrument), "%s", instrument);
    bank->rate   = engine_rate;
    bank->octave = octave_for (instrument, octave_override);
    bank->norm   = 1.0;

    char dir[1100];
    snprintf (dir, sizeof (dir), "%s/%s", root, instrument);

    int listed = -1;
    if (manifest != NULL && manifest[0] != '\0')
        listed = load_from_manifest (bank, manifest, dir, instrument, engine_rate);

    if (listed <= 0) /* no manifest, or it named nothing of ours: scan */
    {
        DIR *d = opendir (dir);
        if (d == NULL)
        {
            snprintf (err, err_len, "cannot open %s", dir);
            return false;
        }
        const struct dirent *e;
        while ((e = readdir (d)) != NULL)
            add_file (bank, dir, e->d_name, engine_rate);
        closedir (d);
    }

    if (bank->n_recs == 0)
    {
        ae_sampler_free (bank);
        snprintf (err, err_len,
                  "no usable %s samples in %s (mono float WAV at %.0f Hz expected)",
                  instrument, dir, engine_rate);
        return false;
    }

    /* Sort zones ascending so the nearest-zone search is a scan over a
       monotone list (and so a debug dump reads as a keyboard). */
    for (int i = 1; i < bank->n_zones; ++i)
        for (int j = i; j > 0 && bank->zones[j] < bank->zones[j - 1]; --j)
        {
            const int tz = bank->zones[j]; bank->zones[j] = bank->zones[j-1]; bank->zones[j-1] = tz;
            const int tm = bank->main_n[j]; bank->main_n[j] = bank->main_n[j-1]; bank->main_n[j-1] = tm;
            const int ts = bank->soft_n[j]; bank->soft_n[j] = bank->soft_n[j-1]; bank->soft_n[j-1] = ts;
            for (int k = 0; k < AE_SMP_MAX_RR; ++k)
            {
                const short a = bank->main_idx[j][k];
                bank->main_idx[j][k] = bank->main_idx[j-1][k];
                bank->main_idx[j-1][k] = a;
                const short c = bank->soft_idx[j][k];
                bank->soft_idx[j][k] = bank->soft_idx[j-1][k];
                bank->soft_idx[j-1][k] = c;
            }
        }
    measure_bank (bank);
    err[0] = '\0';
    return true;
}

void ae_sampler_free (AeSampleBank *bank)
{
    for (int i = 0; i < bank->n_recs; ++i)
        free (bank->recs[i].pcm);
    memset (bank, 0, sizeof (*bank));
}

int ae_sampler_list (const char *root, char names[][32], int max)
{
    if (root == NULL || root[0] == '\0' || max <= 0)
        return 0;
    DIR *d = opendir (root);
    if (d == NULL)
        return 0;

    int n = 0;
    const struct dirent *e;
    while ((e = readdir (d)) != NULL && n < max)
    {
        if (e->d_name[0] == '.')
            continue;
        char sub[1100];
        snprintf (sub, sizeof (sub), "%s/%s", root, e->d_name);
        DIR *s2 = opendir (sub);
        if (s2 == NULL)
            continue; /* not a directory (or unreadable): not an instrument */
        bool any = false;
        const struct dirent *f;
        while (! any && (f = readdir (s2)) != NULL)
        {
            bool soft;
            any = parse_name (f->d_name, &soft) >= 0;
        }
        closedir (s2);
        if (any)
            snprintf (names[n++], 32, "%.31s", e->d_name);
    }
    closedir (d);

    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && strcmp (names[j], names[j - 1]) < 0; --j)
        {
            char t[32];
            memcpy (t, names[j], 32);
            memcpy (names[j], names[j - 1], 32);
            memcpy (names[j - 1], t, 32);
        }
    return n;
}

int ae_sampler_zone (const AeSampleBank *bank, int midi)
{
    int best = -1, bestd = 1 << 20;
    for (int i = 0; i < bank->n_zones; ++i)
    {
        const int d = bank->zones[i] > midi ? bank->zones[i] - midi
                                            : midi - bank->zones[i];
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

const AeSampleRec *ae_sampler_pick (const AeSampleBank *bank, int zone,
                                    double velocity, signed char *rr_state,
                                    unsigned *rng)
{
    if (zone < 0 || zone >= bank->n_zones)
        return NULL;

    /* Layer preference, with fallback in BOTH directions: a quiet strike
       on a zone with no soft files plays main (the original case), and a
       hard strike on a zone with no MAIN files plays soft -- Acoustic
       Instruments 2.1's contrabass pizzicato ships eight soft-only notes
       (rr:0 in its pack.json), and returning NULL there made more than
       half the instrument silent above the layer threshold. The wrong
       timbre is a recording; silence is a hole. */
    bool use_soft = velocity < AE_SMP_SOFT_VEL && bank->soft_n[zone] > 0;
    if (! use_soft && bank->main_n[zone] <= 0 && bank->soft_n[zone] > 0)
        use_soft = true;
    const int   n   = use_soft ? bank->soft_n[zone] : bank->main_n[zone];
    const short *ix = use_soft ? bank->soft_idx[zone] : bank->main_idx[zone];
    if (n <= 0)
        return NULL;
    if (n == 1)
        return &bank->recs[ix[0]];

    /* Round robin, never the immediately-previous pick for this
       (zone, layer): a repeated note reusing its recording machine-guns. */
    const int slot = zone * 2 + (use_soft ? 1 : 0);
    *rng = *rng * 1664525u + 1013904223u;
    int pick = (int) ((*rng >> 8) % (unsigned) n);
    if (pick == rr_state[slot])
        pick = (pick + 1) % n;
    rr_state[slot] = (signed char) pick;
    return &bank->recs[ix[pick]];
}
