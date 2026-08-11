#include "corrector.h"
#include "attack_picks.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AE_PI       3.14159265358979323846
#define AE_SQRT2    1.41421356237309504880
#define AE_MIN_FREQ 65.0    /* default lowest detectable pitch (Hz) */
#define AE_MAX_FREQ 1600.0  /* default highest detectable pitch (Hz) */
#define AE_GATE_RMS 0.0015  /* ~ -56 dBFS noise gate for detection */

static double dclamp (double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* JI landmark set (cents re root), mirroring the UI's JI lane. */
static const struct { int a, b; } k_ji[] = {
    {1,1},{9,8},{7,6},{6,5},{5,4},{4,3},{11,8},{3,2},{8,5},{5,3},{7,4},{9,5},{15,8}
};
#define K_JI_COUNT ((int) (sizeof (k_ji) / sizeof (k_ji[0])))

/* Snap `cents` (re root) to the nearest JI landmark pitch class, keeping the
   octave: circular nearest-PC distance, ties/half go up. */
static double ji_snap_cents (double cents, double period)
{
    double pc = fmod (cents, period);
    if (pc < 0.0)
        pc += period;
    double best_diff = 0.0, best_dist = 1e9;
    for (int i = 0; i < K_JI_COUNT; ++i)
    {
        const double jc = 1200.0 * log2 ((double) k_ji[i].a / (double) k_ji[i].b);
        if (jc > period + 1e-9)
            continue;
        double diff = jc - pc; /* signed move to reach the landmark */
        while (diff > period / 2.0)  diff -= period;
        while (diff < -period / 2.0) diff += period;
        const double dist = fabs (diff);
        if (dist < best_dist || (dist == best_dist && diff > best_diff))
        {
            best_dist = dist;
            best_diff = diff;
        }
    }
    return cents + best_diff;
}

/* ---- synth harmony patches ------------------------------------------------
   Small additive/subtractive recipes tuned to sit behind a voice: partial
   levels sum to ~1, detuned saw pairs give pads their slow chorus movement,
   an optional per-partial vibrato LFO (rate in Hz, depth in cents) adds the
   time-varying detune a static offset cannot, and an optional one-pole
   low-pass (cutoff = lp_mult x fundamental) keeps saws soft. Waves:
   0 = sine, 1 = polyBLEP saw, 2 = polyBLEP square.

   `ensemble` runs the whole synth harmony bus through the triple-chorus
   below -- the string-machine move: a Solina is just sawtooth ranks, and
   ITS sound is the ensemble smearing them. */
typedef struct
{
    float ratio, level, detune_cents;
    int   wave;
    float lfo_hz, lfo_cents;
} AeSynthPartial;
typedef struct
{
    const char    *name;
    int            n;
    AeSynthPartial part[AE_SYNTH_PARTIALS];
    float          lp_mult; /* 0 = no filter */
    int            ensemble;
} AeSynthPatch;

static const AeSynthPatch k_synth_patches[] = {
    { "pad",   3, { { 1.0f, 0.40f, -6.0f, 1, 0, 0 }, { 1.0f, 0.40f, +6.0f, 1, 0, 0 },
                    { 2.0f, 0.14f, +3.0f, 1, 0, 0 } },                          2.5f, 0 },
    { "warm",  3, { { 1.0f, 0.72f,  0.0f, 0, 0, 0 }, { 2.0f, 0.22f, +2.0f, 0, 0, 0 },
                    { 3.0f, 0.07f,  0.0f, 0, 0, 0 } },                          0.0f, 0 },
    { "glass", 3, { { 1.0f, 0.55f,  0.0f, 0, 0, 0 }, { 2.0f, 0.30f, +4.0f, 0, 0, 0 },
                    { 4.0f, 0.16f, -3.0f, 0, 0, 0 } },                          0.0f, 0 },
    { "organ", 4, { { 1.0f, 0.52f,  0.0f, 0, 0, 0 }, { 2.0f, 0.28f,  0.0f, 0, 0, 0 },
                    { 3.0f, 0.16f,  0.0f, 0, 0, 0 }, { 4.0f, 0.09f,  0.0f, 0, 0, 0 } },
                                                                                0.0f, 0 },
    { "sine",  1, { { 1.0f, 0.90f,  0.0f, 0, 0, 0 } },                          0.0f, 0 },
    /* Solina-style string machine: saw ranks at 8' and 4' (plus a whisper of
       2'), each drifting on its own slow/fast vibrato pair, brightened past
       the pad's filter, the ensemble doing the actual string-ness. */
    { "strings", 5, { { 1.0f, 0.26f, -5.0f, 1, 0.7f, 5.0f },
                      { 1.0f, 0.26f, +5.0f, 1, 5.2f, 3.0f },
                      { 2.0f, 0.16f, -3.0f, 1, 0.9f, 5.0f },
                      { 2.0f, 0.11f, +7.0f, 1, 6.1f, 3.0f },
                      { 4.0f, 0.06f,  0.0f, 1, 1.3f, 4.0f } },                  4.0f, 1 },
    /* Vox-humana-ish: a square/saw blend rounded well down, ensemble on --
       the "choir" stop of the same era of machines. */
    { "choir", 4, { { 1.0f, 0.28f, -4.0f, 2, 0.6f, 4.0f },
                    { 1.0f, 0.26f, +4.0f, 1, 4.8f, 3.0f },
                    { 2.0f, 0.18f,  0.0f, 0, 0.0f, 0.0f },
                    { 2.0f, 0.10f, +8.0f, 1, 0.8f, 5.0f } },                    2.2f, 1 },
    /* Bright detuned saw pair, no ensemble: a section pad that cuts. */
    { "brass", 3, { { 1.0f, 0.42f, -3.0f, 1, 4.5f, 2.0f },
                    { 1.0f, 0.42f, +3.0f, 1, 5.5f, 2.0f },
                    { 2.0f, 0.10f,  0.0f, 1, 0.0f, 0.0f } },                    5.0f, 0 },
    /* The Solina with the tone control open: the same ranks as `strings`
       past a much higher cutoff, for when the pad has to sit on top of a
       band rather than under a voice. */
    { "solina bright", 5, { { 1.0f, 0.24f, -5.0f, 1, 0.7f, 5.0f },
                            { 1.0f, 0.24f, +5.0f, 1, 5.2f, 3.0f },
                            { 2.0f, 0.18f, -3.0f, 1, 0.9f, 5.0f },
                            { 2.0f, 0.13f, +7.0f, 1, 6.1f, 3.0f },
                            { 4.0f, 0.10f,  0.0f, 1, 1.3f, 4.0f } },           12.0f, 1 },
    /* Sub octave: one rank an equave down plus its own fundamental, filtered
       well down and deliberately NOT ensembled -- a wandering bass is a
       tuning problem, not an effect. */
    { "bass",  3, { { 0.5f, 0.55f,  0.0f, 1, 0.0f, 0.0f },
                    { 0.5f, 0.18f, +4.0f, 1, 0.45f, 3.0f },
                    { 1.0f, 0.20f,  0.0f, 0, 0.0f, 0.0f } },                    3.0f, 0 },
};
#define K_SYNTH_PATCHES ((int) (sizeof (k_synth_patches) / sizeof (k_synth_patches[0])))

/* A patch's intrinsic RMS per unit amplitude (independent partial phases:
   sine contributes level^2/2, saw level^2/3). Dividing by this lets the
   synth be driven to the same RMS as the sung input, so ghosts sit at the
   vocal's own level exactly like the pitch-shifted ones do -- hg then trims
   from parity, not from an arbitrary synth loudness. */
static double synth_patch_rms (const AeSynthPatch *pat)
{
    double sum = 0.0;
    for (int k = 0; k < pat->n; ++k)
        sum += (double) pat->part[k].level * pat->part[k].level
             * (pat->part[k].wave == 1 ? 1.0 / 3.0
              : pat->part[k].wave == 2 ? 1.0 : 0.5);
    const double rms = sqrt (sum);
    return rms > 1e-6 ? rms : 1.0;
}

int ae_synth_patch_count (void) { return K_SYNTH_PATCHES; }

const char *ae_synth_patch_name (int i)
{
    return i >= 0 && i < K_SYNTH_PATCHES ? k_synth_patches[i].name : "";
}

int ae_synth_patch_find (const char *name)
{
    for (int i = 0; i < K_SYNTH_PATCHES; ++i)
        if (strcmp (name, k_synth_patches[i].name) == 0)
            return i;
    return -1;
}

void ae_corrector_set_synth (AeCorrector *p, int source, int patch,
                             double attack_ms, double release_ms)
{
    p->harm_source = (source == AE_HARM_SRC_SYNTH || source == AE_HARM_SRC_SAMPLE)
                         ? source : AE_HARM_SRC_VOICE;
    p->synth_patch = patch < 0 ? 0
                   : patch >= K_SYNTH_PATCHES ? K_SYNTH_PATCHES - 1 : patch;
    p->synth_attack_ms  = dclamp (attack_ms, 0.0, 5000.0);
    p->synth_release_ms = dclamp (release_ms, 0.0, 10000.0);
}

void ae_corrector_set_voice_sources (AeCorrector *p,
                                     const int sources[AE_HARM_VOICES], int lead)
{
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        const int s = sources[v];
        p->h_source[v] = (s == AE_HARM_SRC_VOICE || s == AE_HARM_SRC_SYNTH
                          || s == AE_HARM_SRC_SAMPLE)
                             ? s : AE_HARM_SRC_DEFAULT;
    }
    p->lead_source = (lead == AE_HARM_SRC_SYNTH || lead == AE_HARM_SRC_SAMPLE)
                         ? lead : AE_HARM_SRC_VOICE;
}

void ae_corrector_set_synth_shape (AeCorrector *p, double ensemble_depth,
                                   double vowel, double tilt_db, int vowel_mode)
{
    p->ensemble_depth = dclamp (ensemble_depth, 0.0, 1.0);
    p->synth_vowel    = dclamp (vowel, 0.0, 1.0);
    p->harm_tilt_db   = dclamp (tilt_db, -12.0, 12.0);
    p->vowel_mode     = vowel_mode == AE_VOWEL_MODE_LPC ? AE_VOWEL_MODE_LPC
                                                        : AE_VOWEL_MODE_VOCODER;
}

void ae_corrector_set_drone (AeCorrector *p, bool on, long long degree)
{
    const long long top = 8LL * (p->edo > 0 ? p->edo : AE_MIN_EDO);
    if (degree < 0)   degree = 0;
    if (degree > top) degree = top;
    p->drone_on = on;
    p->drone_j  = degree;
}

/* Tilt EQ on the harmony bus: split at ~700 Hz and recombine the halves with
   equal and opposite gains, so +6 lifts the top and drops the bottom by the
   same amount and the pivot itself stays put. One shelf pair rather than a
   filter sweep -- "darker / brighter" is a tilt, and a tilt is what stays
   musical at every setting. */
static void render_tilt (AeCorrector *p, float *harm_l, float *harm_r, int n)
{
    if (p->harm_tilt_db != p->tilt_db_cur)
    {
        const double half = p->harm_tilt_db * 0.5;
        p->tilt_g_hi = pow (10.0, half / 20.0);
        p->tilt_g_lo = pow (10.0, -half / 20.0);
        p->tilt_db_cur = p->harm_tilt_db;
    }
    const double a = p->tilt_a, glo = p->tilt_g_lo, ghi = p->tilt_g_hi;
    float *bus[2] = { harm_l, harm_r };
    for (int s = 0; s < 2; ++s)
    {
        double lp = p->tilt_lp[s];
        float *b = bus[s];
        for (int i = 0; i < n; ++i)
        {
            lp += (b[i] - lp) * a;
            b[i] = (float) (glo * lp + ghi * (b[i] - lp));
        }
        p->tilt_lp[s] = lp;
    }
}

/* ---- vowel transfer (channel vocoder) -------------------------------------
   The live voice's spectral envelope, lifted onto the synth: 16 log-spaced
   bandpass bands from 180 Hz to 5.2 kHz (the vowel range -- F1 and F2 both
   sit inside it), an envelope follower per band on the INPUT, and the same
   bank on each synth signal scaled by those envelopes. Sung "ah" -> "oo"
   moves the synth with it.

   Constant-Q bandpasses (RBJ, unity peak gain) sharing one coefficient set
   across all signals: only the four state words per band per signal differ,
   so the whole stage is scalars and no allocation. */
static void voc_prepare (AeCorrector *p)
{
    const double lo = 180.0, hi = 5200.0;
    const double q = 3.5;
    for (int b = 0; b < AE_VOC_BANDS; ++b)
    {
        const double f = lo * pow (hi / lo, (double) b / (AE_VOC_BANDS - 1));
        const double w = 2.0 * AE_PI * f / p->fs;
        const double alpha = sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        p->voc_b0[b] = alpha / a0;          /* b1 = 0, b2 = -b0 */
        p->voc_a1[b] = -2.0 * cos (w) / a0;
        p->voc_a2[b] = (1.0 - alpha) / a0;
    }
    /* Envelope follower: fast enough to catch a consonant, slow enough not
       to buzz at the fundamental. */
    p->voc_atk = 1.0 - exp (-1.0 / (0.008 * p->fs));
    p->voc_rel = 1.0 - exp (-1.0 / (0.045 * p->fs));
    p->voc_ready = true;
}

/* Track the input's band levels for this block (mono, once per block). */
static void voc_analyze (AeCorrector *p, const float *in, int n)
{
    for (int b = 0; b < AE_VOC_BANDS; ++b)
    {
        const double b0 = p->voc_b0[b], a1 = p->voc_a1[b], a2 = p->voc_a2[b];
        double x1 = p->voc_ax1[b], x2 = p->voc_ax2[b];
        double y1 = p->voc_ay1[b], y2 = p->voc_ay2[b];
        double env = p->voc_env[b];
        for (int i = 0; i < n; ++i)
        {
            const double x = in[i];
            const double y = b0 * (x - x2) - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            const double mag = fabs (y);
            env += (mag - env) * (mag > env ? p->voc_atk : p->voc_rel);
        }
        p->voc_ax1[b] = x1; p->voc_ax2[b] = x2;
        p->voc_ay1[b] = y1; p->voc_ay2[b] = y2;
        p->voc_env[b] = env;
    }
}

/* Filter one synth signal through the bank, each band scaled by the input's
   envelope, and crossfade against the dry synth by `amount`. Block-RMS
   normalisation keeps the level where the caller put it -- these patches are
   volume-matched, and a vocoder's raw output level tracks the analysis
   signal, which would double-scale. */
#define AE_VOC_SLICE 64
static void voc_apply (AeCorrector *p, float *buf, int n, int sig, double amount)
{
    if (n <= 0 || amount <= 0.0)
        return;

    double dry[AE_VOC_SLICE], acc[AE_VOC_SLICE];
    double dry_sq = 0.0, wet_sq = 0.0;
    /* The normalisation gain is carried between blocks and smoothed: a
       per-block figure computed from this block's own energy would need a
       second pass over the audio, and stepping it per block would zipper. */
    const double norm = p->voc_norm[sig];

    for (int off = 0; off < n; off += AE_VOC_SLICE)
    {
        const int m = n - off < AE_VOC_SLICE ? n - off : AE_VOC_SLICE;
        for (int i = 0; i < m; ++i)
        {
            dry[i] = buf[off + i];
            acc[i] = 0.0;
        }
        for (int b = 0; b < AE_VOC_BANDS; ++b)
        {
            const double b0 = p->voc_b0[b], a1 = p->voc_a1[b], a2 = p->voc_a2[b];
            const double g = p->voc_env[b];
            double x1 = p->voc_sx1[sig][b], x2 = p->voc_sx2[sig][b];
            double y1 = p->voc_sy1[sig][b], y2 = p->voc_sy2[sig][b];
            for (int i = 0; i < m; ++i)
            {
                const double x = dry[i];
                const double y = b0 * (x - x2) - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x;
                y2 = y1; y1 = y;
                acc[i] += y * g;
            }
            p->voc_sx1[sig][b] = x1; p->voc_sx2[sig][b] = x2;
            p->voc_sy1[sig][b] = y1; p->voc_sy2[sig][b] = y2;
        }
        for (int i = 0; i < m; ++i)
        {
            dry_sq += dry[i] * dry[i];
            wet_sq += acc[i] * acc[i];
            buf[off + i] = (float) (dry[i] + amount * (acc[i] * norm - dry[i]));
        }
    }

    /* Update the carried gain from what this block actually measured. */
    if (wet_sq > 1e-20 && dry_sq > 1e-20)
    {
        const double want = dclamp (sqrt (dry_sq / wet_sq), 0.0, 64.0);
        p->voc_norm[sig] += (want - p->voc_norm[sig]) * 0.25;
    }
}

/* ---- LPC vowel mode (formant-corrected resynthesis) -----------------------
   Where the channel vocoder above measures 16 fixed bands, this estimates
   the vocal tract itself: an order-18 all-pole fit to the current frame,
   which resolves the formants continuously and -- because the fit is
   independent of the excitation -- lets the tract stay put while the synth
   plays a different pitch. That last property is the "formant-corrected"
   part: a harmony a fourth below keeps a human tract instead of sounding
   like slowed tape.

   Levinson-Durbin, returning REFLECTION coefficients. Direct-form
   coefficients are what usually makes this filter explode: interpolating
   two stable polynomials can land on an unstable one. Reflection
   coefficients cannot -- any set with |k| < 1 is stable, and so is anything
   between two such sets -- so those are what get stored, slewed and
   filtered with. Returns false when the frame is too quiet or the fit
   degenerates, which leaves the previous (good) tract in place. */
static bool lpc_analyze_frame (const float *x, int n, double k_out[AE_LPC_ORDER])
{
    double r[AE_LPC_ORDER + 1];
    for (int lag = 0; lag <= AE_LPC_ORDER; ++lag)
    {
        double s = 0.0;
        for (int i = lag; i < n; ++i)
            s += (double) x[i] * x[i - lag];
        r[lag] = s;
    }
    if (r[0] <= 1e-9)
        return false;
    /* Ridge: a hair of white noise on the diagonal keeps the recursion off
       the edge of stability on near-periodic frames (a sung vowel is very
       nearly periodic, which is exactly when this would otherwise fail). */
    r[0] *= 1.0001;
    r[0] += 1e-9;

    double a[AE_LPC_ORDER + 1] = { 0.0 };
    double e = r[0];
    for (int i = 1; i <= AE_LPC_ORDER; ++i)
    {
        double acc = r[i];
        for (int j = 1; j < i; ++j)
            acc -= a[j] * r[i - j];
        const double k = acc / e;
        if (! (k > -0.999 && k < 0.999)) /* NaN-safe */
            return false;
        k_out[i - 1] = k;

        double tmp[AE_LPC_ORDER + 1];
        for (int j = 1; j < i; ++j)
            tmp[j] = a[j] - k * a[i - j];
        for (int j = 1; j < i; ++j)
            a[j] = tmp[j];
        a[i] = k;

        e *= 1.0 - k * k;
        if (e <= 1e-12)
            return false;
    }
    return true;
}

/* The whitened excitation of the live voice: the input run through the
   analysis (FIR) lattice. It carries what the all-pole envelope does not --
   the buzz of the glottis and the noise burst of a consonant -- which is
   what a "t" or an "s" needs to survive into the synth. */
static void lpc_residual (AeCorrector *p, const float *in, float *out, int n)
{
    double *g = p->lpc_inv; /* g[j] holds stage j's backward error, delayed */

    for (int s = 0; s < n; ++s)
    {
        const double x = in[s];
        double f = x;
        /* Stage i reads g_{i-1}[n-1] and produces g_i[n], which stage i+1
           will read NEXT sample -- so each slot must be saved before it is
           overwritten, or the filter reads this sample's value as if it
           were last sample's. */
        double gd = g[0];
        g[0] = x; /* g_0[n] = x[n] */
        for (int i = 1; i <= AE_LPC_ORDER; ++i)
        {
            const double k = p->lpc_k[i - 1];
            const double fi = f - k * gd;
            const double gi = gd - k * f;
            if (i < AE_LPC_ORDER)
            {
                const double saved = g[i];
                g[i] = gi;
                gd = saved;
            }
            f = fi;
        }
        out[s] = (float) f;
    }
}

/* Safety saturator for the LPC wet path. The normaliser below is
   RMS-matched and smoothed across blocks, but an all-pole resonator's
   crest factor can spike within one block when a formant lands on a
   carrier partial -- the average is right while individual peaks run
   several times hot. Linear below the knee (inert in normal operation),
   then a rational curve that approaches knee+1: the wet sample can never
   exceed 2.5 no matter what the lattice rings up to. */
static inline double lpc_sat (double w)
{
    const double knee = 1.5;
    const double aw = fabs (w);
    if (aw <= knee)
        return w;
    const double t = aw - knee;
    const double y = knee + t / (1.0 + t);
    return w < 0.0 ? -y : y;
}

/* Impose the estimated tract on one synth signal: the all-pole (IIR)
   lattice, which is the analysis lattice run backwards. `amount`
   crossfades against the dry synth, and a carried, smoothed normaliser
   holds the level where the volume match put it. */
static void lpc_apply (AeCorrector *p, float *buf, int n, int sig,
                       const float *residual, double amount)
{
    if (n <= 0 || amount <= 0.0 || ! p->lpc_valid)
        return;

    double *b = p->lpc_lat[sig];
    const double norm = p->lpc_norm[sig];
    double dry_sq = 0.0, wet_sq = 0.0;
    double dry_pk = 0.0, wet_pk = 0.0;
    /* A touch of the voice's own excitation rides with the carrier, which
       is what carries consonants; the oscillator alone is purely periodic
       and can only ever sing vowels. */
    const double res_mix = 0.35;

    for (int s = 0; s < n; ++s)
    {
        const double dry = buf[s];
        double f = dry + res_mix * (residual != NULL ? residual[s] : 0.0f);
        for (int i = AE_LPC_ORDER; i >= 1; --i)
        {
            const double k = p->lpc_k[i - 1];
            f += k * b[i - 1];
            b[i] = b[i - 1] - k * f;
        }
        b[0] = f;
        dry_sq += dry * dry;
        wet_sq += f * f;
        const double ad = fabs (dry), aw = fabs (f);
        if (ad > dry_pk) dry_pk = ad;
        if (aw > wet_pk) wet_pk = aw;
        buf[s] = (float) (dry + amount * (lpc_sat (f * norm) - dry));
    }

    /* Peak-aware normaliser: match RMS as the baseline, but never let the
       wet peak exceed the dry peak by more than 6 dB -- a formant filter
       legitimately raises crest factor a little, not by the 15-20 dB a
       resonance parked on a partial can produce. */
    if (wet_sq > 1e-20 && dry_sq > 1e-20)
    {
        double want = sqrt (dry_sq / wet_sq);
        if (wet_pk > 1e-10)
        {
            const double pk_cap = 2.0 * dry_pk / wet_pk;
            if (pk_cap < want)
                want = pk_cap;
        }
        want = dclamp (want, 0.0, 64.0);
        p->lpc_norm[sig] += (want - p->lpc_norm[sig]) * 0.25;
    }
}

/* polyBLEP residual: subtracted at a saw's wrap to band-limit the edge. */
static inline double poly_blep (double t, double dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

void ae_corrector_prepare (AeCorrector *p, double sample_rate, int max_block_size,
                           double min_hz, double max_hz, int quality)
{
    p->fs        = sample_rate > 0.0 ? sample_rate : 44100.0;
    p->max_block = max_block_size > 16 ? max_block_size : 16;

    if (min_hz < 20.0 || min_hz > 500.0)    min_hz = AE_MIN_FREQ;
    if (max_hz < min_hz * 2.0 || max_hz > 4000.0) max_hz = AE_MAX_FREQ;
    p->det_min_hz = min_hz;
    p->det_max_hz = max_hz;

    /* Analysis span: exactly TWO periods of the lowest detectable note --
       the textbook YIN minimum (integration window = tau_max, and every
       x[j], x[j+tau] pair falls inside the frame). The frame used to be
       padded up to a power of two here, which the FFT does not need (it
       pads internally) and which cost real responsiveness: at the guitar
       range the window came out 2.3x the minimum, and both time-to-lock
       on a fresh note and the tracking lag on vibrato scale with the
       window, because the estimate is centred half a window back and a
       new note only reads true once it FILLS the window. Measured on the
       rig's settings: first lock 37.8 -> 17.9 ms, vibrato lag
       17.2 -> 8.6 ms, with the detector-hostile fixture unchanged. */
    const int longest_period = (int) ceil (p->fs / min_hz);
    p->frame_size = 2 * longest_period;
    if (p->frame_size < 512)       p->frame_size = 512;
    if (p->frame_size > (1 << 15)) p->frame_size = 1 << 15;

    /* Detection hop ~5 ms. Halving it was measured and rejected: at the
       shorter window it bought 1.8 ms of lock and 1.3 ms of tracking lag
       for +55% detector CPU (the FFT pads 1232 -> 2048 either way, so the
       per-detection cost does not shrink with the window). This rig runs
       two engine instances; the milliseconds were not worth it. */
    p->hop = (int) (p->fs * 0.005);
    if (p->hop < 128) p->hop = 128;

    p->tau_max = p->frame_size / 2 < longest_period ? p->frame_size / 2 : longest_period;
    if (p->tau_max < 64) p->tau_max = 64;
    p->tau_min = (int) floor (p->fs / max_hz);
    if (p->tau_min < 2) p->tau_min = 2;
    if (p->tau_min > p->tau_max - 1) p->tau_min = p->tau_max - 1;

    /* Latency is the shifter's: output sample i is the corrected input from
       `latency` samples ago, so the dry path is delayed to match. The block
       size (quality preset) is what sets it. */
    p->block_samples = ae_shifter_block_samples (p->fs, quality);

    ae_corrector_free_shifters (p);
    p->shifter = ae_shifter_create (p->fs, p->block_samples);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        p->h_shifter[v] = ae_shifter_create (p->fs, p->block_samples);

    p->latency = p->shifter != NULL ? ae_shifter_latency (p->shifter)
                                    : p->block_samples;

    const int need = p->latency + 2 * p->max_block + p->frame_size + 16;
    p->buf_size = 1;
    while (p->buf_size < need)
        p->buf_size <<= 1;
    p->buf_mask = p->buf_size - 1;

    free (p->in_buf);
    free (p->frame);
    free (p->in_block);
    free (p->wet_buf);
    free (p->voice_buf);
    p->in_buf    = calloc ((size_t) p->buf_size, sizeof (float));
    p->frame     = calloc ((size_t) p->frame_size, sizeof (float));
    p->in_block  = calloc ((size_t) p->max_block, sizeof (float));
    p->wet_buf   = calloc ((size_t) p->max_block, sizeof (float));
    p->voice_buf = calloc ((size_t) p->max_block, sizeof (float));
    free (p->lead_wet);
    p->lead_wet  = calloc ((size_t) p->max_block, sizeof (float));
    free (p->lpc_res);
    p->lpc_res   = calloc ((size_t) p->max_block, sizeof (float));

    /* Harmony sustain: 250 ms is room for a ~90 ms loop plus its crossfade
       even for a very low note (a 40 Hz fundamental is a 25 ms period, so
       two periods plus one of crossfade is 75 ms). */
    free (p->sus_buf);
    free (p->sus_block);
    p->sus_cap   = (int) (0.25 * p->fs) + 8;
    p->sus_buf   = calloc ((size_t) p->sus_cap, sizeof (float));
    p->sus_block = calloc ((size_t) p->max_block, sizeof (float));

    /* Ensemble delay lines: ~40 ms covers the deepest tap plus its sweep. */
    free (p->ens_buf_l);
    free (p->ens_buf_r);
    p->ens_len = 1;
    while (p->ens_len < (int) (0.04 * p->fs) + 8)
        p->ens_len <<= 1;
    p->ens_mask  = p->ens_len - 1;
    p->ens_buf_l = calloc ((size_t) p->ens_len, sizeof (float));
    p->ens_buf_r = calloc ((size_t) p->ens_len, sizeof (float));

    /* IR points: the 2 s ceiling at the engine rate. Created once per
       prepare; irc_point_process is allocation-free from here on. */
    irc_point_destroy (p->ir_lead);
    irc_point_destroy (p->ir_harm[0]);
    irc_point_destroy (p->ir_harm[1]);
    p->ir_lead    = irc_point_create ((int) (2.0 * p->fs), p->max_block, p->fs);
    p->ir_harm[0] = irc_point_create ((int) (2.0 * p->fs), p->max_block, p->fs);
    p->ir_harm[1] = irc_point_create ((int) (2.0 * p->fs), p->max_block, p->fs);

    voc_prepare (p); /* bandpass coefficients follow the sample rate */
    /* Tilt pivot: 700 Hz, the crossover a "darker/brighter" control wants --
       low enough that dropping the top keeps the fundamentals, high enough
       that lifting it is air rather than honk. */
    p->tilt_a = 1.0 - exp (-2.0 * AE_PI * 700.0 / p->fs);
    p->tilt_db_cur = 1e9; /* force a gain rebuild on the first block */

    ae_yin_prepare (&p->detector, p->fs, p->frame_size, min_hz, max_hz);

    if (p->edo == 0) /* first prepare on a zeroed struct: neutral defaults */
    {
        p->edo           = 12;
        p->retune_ms     = 20.0;
        p->transition_ms = 50.0;
        p->amount        = 1.0;
        p->ref_hz        = AE_REFERENCE_C0_HZ;
        p->period_cents  = 1200.0;
        /* Zero means "voice" for these enums, which would silently pin every
           voice to the shifter and ignore the global switch; the sentinel is
           what makes the global switch the default. Full ensemble depth
           likewise: a zeroed struct must behave as the patch intends. */
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            p->h_source[v] = AE_HARM_SRC_DEFAULT;
        p->ensemble_depth = 1.0;
    }

    ae_corrector_reset (p);
}

void ae_corrector_reset (AeCorrector *p)
{
    if (p->in_buf != NULL)
        memset (p->in_buf, 0, (size_t) p->buf_size * sizeof (float));

    p->in_write        = 0;
    p->last_detect_at  = 0;
    p->shift_semitones = 0.0;

    p->voiced         = false;
    p->primed         = false;
    p->out_cents      = 0.0;
    p->centre_cents   = 0.0;
    p->expr_cents     = 0.0;
    if (p->expression <= 0.0) p->expression = 1.0;
    p->v_gain         = 0.0;
    p->in_level       = 0.0;
    p->target_j       = 0;
    p->target_valid   = false;
    p->prev_pair_valid = false;
    p->prev_det_cents = p->prev_tgt_cents = 0.0;
    p->in_transition  = false;
    p->sustain_s      = 0.0;

    /* Unity until a controller says otherwise, and STARTING at unity rather
       than ramping to it: a fresh engine must not fade its harmony in. */
    if (p->harm_master <= 0.0)
        p->harm_master = 1.0;
    p->harm_master_cur = p->harm_master;
    p->lead_on         = true;
    p->formant_hold = true;
    p->midi_fold    = true;
    atomic_store_explicit (&p->shift_st_out, 0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->shift_st_min, 0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->shift_st_max, 0.0f, memory_order_relaxed);
    p->atk_fast   = p->atk_slow = 0.0;
    p->atk_refract = 0;
    p->atk_active = 0;
    p->atk_smp    = NULL;
    p->atk_amp    = 0.0;
    p->atk_rng    = 0x9e3779b9u;
    p->atk_last_range = -1;
    atomic_store_explicit (&p->smp_live, -1, memory_order_relaxed);
    if (p->smp_mix <= 0.0) p->smp_mix = 1.0;
    if (p->smp_vel_fixed == 0.0) p->smp_vel_fixed = -1.0;
    p->smp_rng = 0x2545f491u;
    if (p->smp_octave == 0) p->smp_octave = AE_SMP_OCTAVE_AUTO;
    /* Voices default to "follow the global source" -- the documented
       meaning of AE_HARM_SRC_DEFAULT. A zeroed struct would otherwise read
       as an explicit per-voice AE_HARM_SRC_VOICE override and silently
       pin every voice to the shifter. */
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        p->h_source[v] = AE_HARM_SRC_DEFAULT;
    p->smp_gain_a = 1.0 - exp (-1.0 / (0.015 * p->fs));
    p->onset_pulse = false;
    p->vel_win = 0; p->vel_peak = 0.0;
    /* Start at the floor, so the reference is OBSERVED rather than assumed.
       Seeding it with a plausible "playing hard" level instead sounds
       reasonable and is not: the seed then outranks the player for as long
       as it takes to decay, and every note until then is scored against a
       number nobody played. Starting low costs one note -- the first of a
       set reads as the loudest so far, because it is -- and the first
       genuinely hard note corrects it for good. */
    p->vel_ref = AE_VEL_REF_MIN;
    p->vel_ref_fixed = -1.0; /* observe until a host says otherwise */
    memset (p->smp_rr, -1, sizeof (p->smp_rr));
    memset (p->smp, 0, sizeof (p->smp));
    memset (p->smp_cur, 0, sizeof (p->smp_cur));
    memset (p->smp_pending, 0, sizeof (p->smp_pending));
    memset (p->smp_wait, 0, sizeof (p->smp_wait));
    memset (p->smp_env, 0, sizeof (p->smp_env));
    atomic_store_explicit (&p->smp_vel_out, 0.7f, memory_order_relaxed);
    atomic_store_explicit (&p->smp_vel_ref, (float) p->vel_ref, memory_order_relaxed);
    p->lead_env = 0.0;
    p->atk_last_dir   = 0;
    if (p->atk_gain <= 0.0)
        p->atk_gain = pow (10.0, -26.0 / 20.0); /* Xentar's shipped 5% */
    p->sus_len         = 0;
    p->sus_read        = 0;
    p->sus_mix         = 0.0;
    p->last_voiced_hz  = 0.0;
    memset (p->rel_det, 0, sizeof (p->rel_det));
    memset (p->rel_rms, 0, sizeof (p->rel_rms));
    p->rel_pos = 0;
    p->atk_armed = true;
    p->hold_latched    = false;
    p->hold_level      = 0.0;

    for (int i = 0; i < AE_MAX_EDO; ++i)
        p->enabled_deg[i] = true; /* default: every degree usable */

    ae_shifter_reset (p->shifter);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        ae_shifter_reset (p->h_shifter[v]);
        p->h_semitones[v] = 0.0;
        p->h_active[v]    = false;
        p->h_fed[v]       = false;
        p->h_mix[v]       = 0.0;
        p->h_cents_t[v]   = 0.0;
        p->h_cents_cur[v] = 0.0;
        p->h_glide_rate[v] = 0.0;
        p->h_glide_tgt[v] = 0.0;
        p->h_glide_valid[v] = false;
        p->s_cents[v]     = 0.0;
        p->s_env[v]       = 0.0;
        p->s_lp[v]        = 0.0;
        for (int k = 0; k < AE_SYNTH_PARTIALS; ++k)
        {
            /* Stagger the start phases: identical phases across voices make
               the first attack of a stacked chord sum to a click. */
            p->s_phase[v][k] = (double) (v * AE_SYNTH_PARTIALS + k)
                             / (double) (AE_HARM_VOICES * AE_SYNTH_PARTIALS);
            p->s_lfo[v][k]   = 2.0 * AE_PI * p->s_phase[v][k];
        }
        atomic_store_explicit (&p->h_deg_out[v], AE_HARM_DEG_OFF, memory_order_relaxed);
    }
    p->drone_env   = 0.0;
    p->drone_cents = 0.0;
    p->drone_lp    = 0.0;
    for (int k = 0; k < AE_SYNTH_PARTIALS; ++k)
    {
        p->drone_phase[k] = (double) k / (double) AE_SYNTH_PARTIALS;
        p->drone_lfo[k]   = 2.0 * AE_PI * p->drone_phase[k];
    }
    if (p->ir_lead != NULL)    irc_point_reset (p->ir_lead);
    if (p->ir_harm[0] != NULL) irc_point_reset (p->ir_harm[0]);
    if (p->ir_harm[1] != NULL) irc_point_reset (p->ir_harm[1]);

    for (int b = 0; b < AE_VOC_BANDS; ++b)
    {
        p->voc_env[b] = 0.0;
        p->voc_ax1[b] = p->voc_ax2[b] = p->voc_ay1[b] = p->voc_ay2[b] = 0.0;
        for (int s = 0; s < AE_VOC_SIGNALS; ++s)
        {
            p->voc_sx1[s][b] = p->voc_sx2[s][b] = 0.0;
            p->voc_sy1[s][b] = p->voc_sy2[s][b] = 0.0;
        }
    }
    for (int s = 0; s < AE_VOC_SIGNALS; ++s)
        p->voc_norm[s] = 1.0;
    p->tilt_lp[0] = p->tilt_lp[1] = 0.0;

    p->lpc_valid = false;
    for (int i = 0; i < AE_LPC_ORDER; ++i)
        p->lpc_k[i] = p->lpc_k_t[i] = 0.0;
    for (int i = 0; i <= AE_LPC_ORDER; ++i)
        p->lpc_inv[i] = 0.0;
    for (int sg = 0; sg < AE_VOC_SIGNALS; ++sg)
    {
        p->lpc_norm[sg] = 1.0;
        for (int i = 0; i <= AE_LPC_ORDER; ++i)
            p->lpc_lat[sg][i] = 0.0;
    }
    if (p->lpc_res != NULL)
        memset (p->lpc_res, 0, (size_t) p->max_block * sizeof (float));

    if (p->ens_buf_l != NULL)
        memset (p->ens_buf_l, 0, (size_t) p->ens_len * sizeof (float));
    if (p->ens_buf_r != NULL)
        memset (p->ens_buf_r, 0, (size_t) p->ens_len * sizeof (float));
    p->ens_write = 0;
    /* Three taps, each with its own slow and fast LFO, started a third of a
       cycle apart so the taps never sweep in unison (that would be vibrato,
       not ensemble). */
    for (int i = 0; i < 6; ++i)
        p->ens_lfo[i] = 2.0 * AE_PI * (double) (i % 3) / 3.0;

    atomic_store_explicit (&p->detected_hz_out, 0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->target_hz_out,   0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->voiced_out,      false, memory_order_relaxed);
}

void ae_corrector_free_shifters (AeCorrector *p)
{
    ae_shifter_destroy (p->shifter);
    p->shifter = NULL;
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        ae_shifter_destroy (p->h_shifter[v]);
        p->h_shifter[v] = NULL;
    }
}

void ae_corrector_free (AeCorrector *p)
{
    free (p->in_buf);
    free (p->frame);
    free (p->in_block);
    free (p->wet_buf);
    free (p->voice_buf);
    free (p->lead_wet);
    p->lead_wet = NULL;
    p->in_buf = p->frame = p->in_block = p->wet_buf = p->voice_buf = NULL;
    free (p->lpc_res);
    p->lpc_res = NULL;
    free (p->sus_buf);
    free (p->sus_block);
    p->sus_buf = p->sus_block = NULL;
    p->sus_cap = p->sus_len = 0;
    free (p->ens_buf_l);
    free (p->ens_buf_r);
    p->ens_buf_l = p->ens_buf_r = NULL;
    p->ens_len = p->ens_mask = 0;
    ae_corrector_free_shifters (p);
    irc_point_destroy (p->ir_lead);
    irc_point_destroy (p->ir_harm[0]);
    irc_point_destroy (p->ir_harm[1]);
    p->ir_lead = p->ir_harm[0] = p->ir_harm[1] = NULL;
    ae_yin_free (&p->detector);
    ae_sampler_free (&p->smp_bank[0]);
    ae_sampler_free (&p->smp_bank[1]);
    atomic_store_explicit (&p->smp_live, -1, memory_order_relaxed);
}

bool ae_corrector_load_ir (AeCorrector *p, int point, const float *ir_l,
                           const float *ir_r, int len, double predelay_ms)
{
    if (point == 0)
        return p->ir_lead != NULL
            && irc_point_load (p->ir_lead, ir_l, len, predelay_ms);
    if (p->ir_harm[0] == NULL || p->ir_harm[1] == NULL)
        return false;
    if (irc_point_busy (p->ir_harm[0]) || irc_point_busy (p->ir_harm[1]))
        return false;
    /* Both sides must take the swap, or the stereo image would smear one
       space against another for 30 ms. */
    const bool a = irc_point_load (p->ir_harm[0], ir_l, len, predelay_ms);
    const bool b = irc_point_load (p->ir_harm[1],
                                   ir_r != NULL ? ir_r : ir_l, len, predelay_ms);
    return a && b;
}

void ae_corrector_set_ir_params (AeCorrector *p, int point, double mix,
                                 double gain_db, bool on)
{
    if (point == 0)
    {
        if (p->ir_lead != NULL)
            irc_point_set (p->ir_lead, mix, gain_db, on);
        return;
    }
    if (p->ir_harm[0] != NULL)
        irc_point_set (p->ir_harm[0], mix, gain_db, on);
    if (p->ir_harm[1] != NULL)
        irc_point_set (p->ir_harm[1], mix, gain_db, on);
}

void ae_corrector_set_harmony (AeCorrector *p, bool on, int lock,
                           const AeHarmVoice voices[AE_HARM_VOICES])
{
    /* Rising edge of the master switch: forget every portamento position.
       A voice can still be ringing out its release from before the OFF
       (long releases outlive a quick toggle, and h_glide_valid only clears
       once the tail has fully died), and without this the first note after
       re-enable would SLIDE in from wherever harmony last sang -- the
       exact swoop the arriving-from-silence rule exists to prevent.
       Turning harmony on is a fresh start: the first note lands on pitch. */
    if (on && ! p->harm_on)
        for (int v = 0; v < AE_HARM_VOICES; ++v)
        {
            p->h_glide_valid[v] = false;
            p->h_glide_rate[v]  = 0.0;
        }

    p->harm_on   = on;
    p->harm_lock = lock < 0 ? 0 : (lock > 2 ? 2 : lock);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        AeHarmVoice hv = voices[v];
        if (hv.ext_oct < 0) hv.ext_oct = 0;
        if (hv.ext_oct > 2) hv.ext_oct = 2;
        if (hv.pan < -1.0) hv.pan = -1.0;
        if (hv.pan >  1.0) hv.pan =  1.0;
        if (hv.gain < 0.0) hv.gain = 0.0;
        if (hv.detune_cents < -100.0) hv.detune_cents = -100.0;
        if (hv.detune_cents >  100.0) hv.detune_cents =  100.0;
        p->harm[v] = hv;
        /* Constant-power pan, normalised so DEAD CENTRE IS UNITY IN BOTH
           CHANNELS. The lead is mono and reaches L and R at full level, so
           the textbook cos/sin law (0.707 a side) would put a centred ghost
           a fixed 3 dB under the lead -- a deficit the per-voice trim then
           has to spend half its range undoing. The sqrt(2) here is what
           makes "gain 0 dB" mean "at the lead's level", which is what an
           operator reasonably expects it to mean. Power stays constant
           across the sweep: hard over is sqrt(2) into one side, the same
           total as unity into both. */
        const double a = (hv.pan + 1.0) * (AE_PI / 4.0);
        p->h_gl[v] = hv.gain * cos (a) * AE_SQRT2;
        p->h_gr[v] = hv.gain * sin (a) * AE_SQRT2;
    }
}

/* Lift a loopable slice out of the end of the note. A whole number of pitch
   periods, so the wrap is already near-continuous before anything is
   crossfaded; then the tail is crossfaded against the material that
   PRECEDES the slice, which is what makes buf[len-1] run into buf[0]
   instead of stepping. Allocation-free; runs on the audio thread at a note
   boundary, which is a few thousand copies once. */
static void capture_sustain (AeCorrector *p, long long end)
{
    p->sus_len = 0;
    if (p->sus_buf == NULL || p->last_voiced_hz <= 0.0)
        return;

    const int period = (int) (p->fs / p->last_voiced_hz + 0.5);
    if (period < 8)
        return;

    /* ~90 ms of loop: long enough to carry the timbre and any vibrato
       period, short enough that it is still the same note throughout. */
    int k = (int) ((0.090 * p->fs) / period + 0.5);
    if (k < 2) k = 2;
    int len = k * period;
    const int xf = period; /* one period of crossfade */
    while (k > 2 && len + xf > p->sus_cap)
        len = --k * period;
    if (len + xf > p->sus_cap || end - (long long) (len + xf) < 0
        || p->in_write - (end - (long long) (len + xf)) > (long long) p->buf_size)
        return;

    const long long s0 = end - len;
    for (int i = 0; i < len; ++i)
        p->sus_buf[i] = p->in_buf[(s0 + i) & p->buf_mask];
    for (int j = 0; j < xf; ++j)
    {
        const double w = (double) j / (double) xf;
        const int    i = len - xf + j;
        p->sus_buf[i] = (float) (p->sus_buf[i] * (1.0 - w)
                               + p->in_buf[(s0 - xf + j) & p->buf_mask] * w);
    }
    p->sus_len  = len;
    p->sus_read = 0;
}

void ae_corrector_set_sample (AeCorrector *p, double mix, double velocity,
                              bool ring)
{
    p->smp_mix = mix < 0.0 ? 0.0 : (mix > 1.0 ? 1.0 : mix);
    p->smp_vel_fixed = velocity < 0.0 ? -1.0 : (velocity > 1.0 ? 1.0 : velocity);
    /* Turning let-ring OFF must not strand notes that are already ringing:
       retire them the way a strike would, so the switch is heard as the
       damper coming down rather than as nothing until the next note. */
    if (p->smp_ring && ! ring)
        for (int v = 0; v <= AE_HARM_VOICES; ++v)
            for (int k = 0; k < AE_SMP_SLOTS; ++k)
                if (k != p->smp_cur[v] && p->smp[v][k].rec != NULL)
                    p->smp[v][k].retiring = true;
    p->smp_ring = ring;
}

/* Supply the velocity reference instead of observing one. `ref_lin` is a
   linear peak; negative means observe. Applied on the next block, so a
   host may re-assert it per phrase without clicking anything: the
   reference scales velocities, it is not in the audio path. */
void ae_corrector_set_vel_ref (AeCorrector *p, double ref_lin)
{
    p->vel_ref_fixed = ref_lin < 0.0 ? -1.0 : (ref_lin > 1.0 ? 1.0 : ref_lin);
    if (p->vel_ref_fixed >= 0.0)
    {
        /* Take effect now rather than at the next onset, so a status read
           straight after the write agrees with what was asked for. */
        p->vel_ref = p->vel_ref_fixed;
        atomic_store_explicit (&p->smp_vel_ref, (float) p->vel_ref,
                               memory_order_relaxed);
    }
}

/* The LEAD's own envelope. Separate control from the harmony's because the
   two shape different things -- see the note in the header. */
void ae_corrector_set_lead_env (AeCorrector *p, double attack_ms,
                                double release_ms)
{
    p->lead_attack_ms  = dclamp (attack_ms, 0.0, 5000.0);
    p->lead_release_ms = dclamp (release_ms, 0.0, 10000.0);
}

void ae_corrector_set_sample_octave (AeCorrector *p, int semitones)
{
    p->smp_octave = semitones;
}

bool ae_corrector_load_samples (AeCorrector *p, const char *root,
                                const char *instrument, const char *manifest,
                                char *err, size_t err_len)
{
    const int live = atomic_load_explicit (&p->smp_live, memory_order_relaxed);
    const int idle = live == 0 ? 1 : 0;
    /* The idle slot has been dead since the previous swap, which held off
       long enough for the audio thread to turn a block over -- so freeing
       and refilling it cannot pull a buffer out from under a voice. */
    if (! ae_sampler_load (&p->smp_bank[idle], root, instrument, manifest,
                           p->fs, p->smp_octave, err, err_len))
        return false;                     /* the running bank is untouched */
    atomic_store_explicit (&p->smp_gen,
                           atomic_load_explicit (&p->smp_gen, memory_order_relaxed) + 1,
                           memory_order_relaxed);
    atomic_store_explicit (&p->smp_live, idle, memory_order_release);
    memset (p->smp_rr, -1, sizeof (p->smp_rr));
    return true;
}

/* Peak amplitude -> strike level, RELATIVE to how hard this player plays
   when playing hard.

   This used to be an absolute map: -40 dBFS is 0, full scale is 1. That
   design was field-verified wrong, and the reason is worth keeping written
   down because it is invisible from a synthetic test. A real interface is
   set up with 12-20 dB of headroom, so hard playing peaks at -12..-20
   dBFS and NEVER approaches full scale. Under the absolute map every
   velocity on the rig therefore sat in the bottom half of its range --
   hard playing read as medium, and anything played quietly fell off the
   bottom and vanished. The map was measuring the gain staging, not the
   playing.

   So the reference is the CALLER's, not the format's: the loudest the
   player has been recently. `ref = max(refPeak, peak)` means the map never
   asks for more than unity even when the reference is stale low, a 24 dB
   window below it spans the useful dynamic range of a plucked or struck
   note, and the 0.2 floor keeps a note the detector CONFIRMED from being
   struck near-silent -- a quiet note is a quiet note, not an absent one.

   Shared by the estimate at the strike and by the measurement that refines
   it, so the two stay on one scale by construction. */
static double vel_from_peak (const AeCorrector *p, double peak)
{
    if (peak <= 1e-9)
        return AE_VEL_FLOOR;
    const double ref   = p->vel_ref > peak ? p->vel_ref : peak;
    const double below = 20.0 * log10 (peak / ref); /* <= 0 by construction */
    double t = 1.0 + below / AE_VEL_WINDOW_DB;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return AE_VEL_FLOOR + (1.0 - AE_VEL_FLOOR) * t;
}

/* Strike one voice: pick the recording, set the read cursor and the
   fractional rate. The OLD slot is left ringing on a 6 ms fade rather than
   cut, which is the sampler equivalent of Xentar's node-swap retrigger --
   never interrupt a live voice, crossfade past it. */
static void sample_strike (AeCorrector *p, int v, double hz, double vel)
{
    const int live = atomic_load_explicit (&p->smp_live, memory_order_acquire);
    if (live < 0 || hz <= 0.0)
        return;
    const AeSampleBank *bank = &p->smp_bank[live];
    if (bank->n_recs == 0)
        return;

    const int midi = (int) lround (12.0 * log2 (hz / 440.0) + 69.0);
    const int zone = ae_sampler_zone (bank, midi);
    const AeSampleRec *rec = ae_sampler_pick (bank, zone, vel, p->smp_rr, &p->smp_rng);
    if (rec == NULL)
        return;

    /* Choose the slot to strike into: a free one if there is one, else the
       one furthest through its recording -- the oldest, and by then the
       quietest, so stealing it is the least audible steal on offer. */
    int nxt = -1;
    double worst = -1.0;
    for (int k = 0; k < AE_SMP_SLOTS; ++k)
    {
        if (p->smp[v][k].rec == NULL) { nxt = k; break; }
        const double prog = p->smp[v][k].rec->len > 0
            ? p->smp[v][k].pos / (double) p->smp[v][k].rec->len : 1.0;
        if (prog > worst) { worst = prog; nxt = k; }
    }

    /* Damp-on-repitch retires everything already sounding across the 6 ms
       fade -- crossfade past a live voice, never cut it. Let-ring leaves
       them alone to finish on their own decay, which is the whole point:
       the next strike is a NEW string, not this one being re-fretted. */
    if (! p->smp_ring)
    {
        for (int k = 0; k < AE_SMP_SLOTS; ++k)
            if (k != nxt && p->smp[v][k].rec != NULL)
                p->smp[v][k].retiring = true;
    }
    else
        /* Let-ring is bounded: from the moment it is superseded a note
           decays under the release ceiling. Its natural end still applies
           if that comes sooner. */
        for (int k = 0; k < AE_SMP_SLOTS; ++k)
            if (k != nxt && p->smp[v][k].rec != NULL)
                p->smp[v][k].releasing = true;
    p->smp_cur[v] = nxt;

    const double rec_hz = 440.0 * pow (2.0, (rec->midi - 69) / 12.0);
    p->smp[v][nxt].rec  = rec;
    p->smp[v][nxt].pos  = 0.0;
    /* Fractional and UNQUANTISED -- that is what lands a 22-EDO degree
       exactly off a 12-per-octave map. */
    p->smp[v][nxt].rate = hz / rec_hz;
    p->smp[v][nxt].gain   = vel;
    p->smp[v][nxt].gain_t = vel;
    /* The bank's measured level travels with the strike, so a slot still
       ringing from the previous instrument keeps ITS normalisation. */
    p->smp[v][nxt].norm   = bank->norm;
    p->smp[v][nxt].fade = 1.0;
    p->smp[v][nxt].retiring  = false;
    p->smp[v][nxt].releasing = false;
    p->smp[v][nxt].renv      = 1.0;
    p->smp[v][nxt].gen  = atomic_load_explicit (&p->smp_gen, memory_order_relaxed);
}

/* Read one slot into the accumulator. Linear interpolation at the slot's
   own rate; a slot that runs off the end of its recording simply stops. */
static double sample_tick (AeCorrector *p, int v, int slot)
{
    if (p->smp[v][slot].rec == NULL)
        return 0.0;
    if (p->smp[v][slot].gen != atomic_load_explicit (&p->smp_gen, memory_order_relaxed))
    {
        p->smp[v][slot].rec = NULL; /* the bank moved under it */
        return 0.0;
    }
    const AeSampleRec *r = p->smp[v][slot].rec;
    const int k = (int) p->smp[v][slot].pos;
    if (k >= r->len - 1)
    {
        p->smp[v][slot].rec = NULL;
        return 0.0;
    }
    const double f = p->smp[v][slot].pos - k;
    const double x = (1.0 - f) * r->pcm[k] + f * r->pcm[k + 1];
    p->smp[v][slot].pos += p->smp[v][slot].rate;
    /* The refinement RAMPS (~15 ms) rather than stepping: a level
       correction that slides is heard as the note settling, a step is
       heard as a second event. */
    p->smp[v][slot].gain += (p->smp[v][slot].gain_t - p->smp[v][slot].gain)
                          * p->smp_gain_a;
    return x * p->smp[v][slot].gain * p->smp[v][slot].fade
             * p->smp[v][slot].renv * p->smp[v][slot].norm;
}

/* Is every slot of this voice silent? Cheaper than it looks and asked once
   per block, not per sample. */
static bool sample_voice_idle (const AeCorrector *p, int v)
{
    for (int k = 0; k < AE_SMP_SLOTS; ++k)
        if (p->smp[v][k].rec != NULL)
            return false;
    return true;
}

/* One sample of a whole voice: every slot summed, retiring ones walked
   down the 6 ms damp fade, superseded ones (let-ring) walked down the
   release ceiling `rel_a` -- the same coefficient the voice's envelope
   uses, so "release" means one thing. A slot is freed at -60 dB; below
   that it is only spending cycles. */
static double sample_mix_slots (AeCorrector *p, int v, double fade_step,
                                double rel_a)
{
    double x = 0.0;
    for (int k = 0; k < AE_SMP_SLOTS; ++k)
    {
        if (p->smp[v][k].rec == NULL)
            continue;
        x += sample_tick (p, v, k);
        if (p->smp[v][k].retiring)
        {
            p->smp[v][k].fade -= fade_step;
            if (p->smp[v][k].fade <= 0.0)
            {
                p->smp[v][k].fade    = 0.0;
                p->smp[v][k].rec     = NULL;
                p->smp[v][k].retiring = false;
            }
        }
        else if (p->smp[v][k].releasing)
        {
            p->smp[v][k].renv -= p->smp[v][k].renv * rel_a;
            if (p->smp[v][k].renv < 1e-3)
            {
                p->smp[v][k].rec       = NULL;
                p->smp[v][k].releasing = false;
            }
        }
    }
    return x;
}

/* Re-pitch the slot struck most recently -- and only that one. The others
   are notes already sounding: under let-ring they keep their own pitch,
   which is what makes the ring a chord rather than a glissando. */
static void sample_repitch (AeCorrector *p, int v, double hz)
{
    const int cur = p->smp_cur[v];
    if (hz <= 0.0 || p->smp[v][cur].rec == NULL)
        return;
    const AeSampleRec *r = p->smp[v][cur].rec;
    p->smp[v][cur].rate = hz / (440.0 * pow (2.0, (r->midi - 69) / 12.0));
}

void ae_corrector_set_attack (AeCorrector *p, int mode, double gain_lin)
{
    p->atk_mode = mode < 0 ? 0 : (mode > 3 ? 3 : mode);
    p->atk_gain = gain_lin < 0.0 ? 0.0 : (gain_lin > 16.0 ? 16.0 : gain_lin);
}

/* Is there a voice in the path for the attack sound to sit under? BOTH
   sources, like the envelope: a shifted ghost's onset is as synthetic as a
   synth ghost's -- the shifter's latency and the mix ramp swallow the real
   pick -- so it gets the cover too. Only a rig with nothing on the bus at
   all (harmony off, shifted lead) gets no hits. */
static bool attack_relevant (const AeCorrector *p)
{
    if (p->lead_source == AE_HARM_SRC_SYNTH)
        return true;
    if (! p->harm_on)
        return false;
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        if (ae_harm_voice_on (&p->harm[v]))
            return true;
    return false;
}

static inline double atk_rand01 (AeCorrector *p) /* LCG: RT-safe, seedable */
{
    p->atk_rng = p->atk_rng * 1664525u + 1013904223u;
    return (double) (p->atk_rng >> 8) / 16777216.0;
}

/* Onset -> one hit. The trigger is ENERGY appearing -- a fast follower
   overtaking a slow one -- which fires several hops before YIN has settled
   on a pitch; that head start is the entire point. The pick variant is the
   Xentar model: range from the last known pitch, direction from the
   economy-picking state machine (same range alternates; crossing to a
   higher range continues DOWN, to a lower one UP -- the pick keeps
   travelling), +-8% rate and +-26% level jitter per hit so a run of notes
   reads as a hand, not a sampler. */
static void attack_trigger (AeCorrector *p)
{
    p->atk_active = p->atk_mode;
    p->atk_amp    = p->atk_fast;
    p->atk_jit    = 1.0 + (atk_rand01 (p) - 0.5) * 2.0 * 0.26;
    p->atk_smp    = NULL;

    if (p->atk_mode == AE_ATK_PICK)
    {
        const int range = p->last_voiced_hz <= 0.0 ? 1
                        : p->last_voiced_hz < 130.0 ? 0
                        : p->last_voiced_hz < 220.0 ? 1 : 2;
        int dir;
        if (range == p->atk_last_range)
            dir = ! p->atk_last_dir;              /* same string: alternate */
        else if (p->atk_last_range < 0)
            dir = 0;                              /* very first note: down */
        else
            dir = range > p->atk_last_range ? 0 : 1; /* economy picking */
        p->atk_last_range = range;
        p->atk_last_dir   = dir;

        p->atk_smp     = ae_attack_picks[range][dir].pcm;
        p->atk_smp_len = ae_attack_picks[range][dir].len;
        p->atk_smp_rms = ae_attack_picks[range][dir].rms;
        p->atk_pos     = 0.0;
        p->atk_rate    = (1.0 + (atk_rand01 (p) - 0.5) * 2.0 * 0.08)
                       * AE_ATTACK_PICK_RATE / p->fs;
    }
    else if (p->atk_mode == AE_ATK_NOISE)
    {
        p->atk_env   = 1.0;
        p->atk_env_a = exp (-1.0 / (0.020 * p->fs)); /* ~60 ms audible */
        p->atk_hp_x  = p->atk_hp_y = 0.0;
    }
    else /* AE_ATK_CLICK */
    {
        p->atk_env    = 1.0;
        p->atk_env_a  = exp (-1.0 / (0.008 * p->fs));
        p->atk_ph     = 0.0;
        p->atk_ph_inc = 2.0 * AE_PI * (1800.0 + atk_rand01 (p) * 800.0) / p->fs;
    }
}

/* Detect this block's onset (always, so the followers stay warm) and, if a
   hit is playing, add it to the harmony bus -- BEFORE the bus IR, tilt and
   master, so it sits in the same space as the ghosts it covers, but through
   no envelope and no vocoder: its own gain is the whole of its level law. */
/* The note EDGE, and the strike level that goes with it. Runs before
   anything that needs either, because both the attack sound and the sample
   strike must fire before the detector has a pitch -- that head start is
   the whole point of both features.

   Velocity is a MEASUREMENT, not a constant: the peak over the first 30 ms
   from the foot of the attack, mapped across 40 dB. The strike itself
   cannot wait for that window (waiting would give back the latency), so a
   voice is struck at the fast follower's reading and the window's verdict
   refines it -- which is audible as the note settling, not as a re-strike. */
static void detect_onset (AeCorrector *p, int num_samples)
{
    double sum = 0.0, peak = 0.0;
    for (int i = 0; i < num_samples; ++i)
    {
        const double a = fabs ((double) p->in_block[i]);
        if (a > peak) peak = a;
        sum += (double) p->in_block[i] * p->in_block[i];
    }
    const double rms = sqrt (sum / num_samples);

    /* A supplied reference is held exactly, neither decayed nor raised:
       the host is asserting what hard playing IS on this rig, and an
       engine that drifted off it would be quietly disagreeing. Otherwise
       let the observed reference forget a loud passage -- decay only, since
       it is raised by measured onsets below and never by sustain, so a long
       held note cannot talk itself into being a hard strike. */
    if (p->vel_ref_fixed >= 0.0)
        p->vel_ref = p->vel_ref_fixed;
    else
    {
        p->vel_ref *= exp (-(double) num_samples / (AE_VEL_REF_TAU_S * p->fs));
        if (p->vel_ref < AE_VEL_REF_MIN) p->vel_ref = AE_VEL_REF_MIN;
    }
    /* Publish every block, not only when an onset closes its window: the
       reference decays continuously, so a readout that only refreshed on
       strikes would sit frozen at the last note's value through every
       silence -- exactly when someone is looking at it to work out why a
       velocity came out the way it did. */
    atomic_store_explicit (&p->smp_vel_ref, (float) p->vel_ref,
                           memory_order_relaxed);

    const double a_fast = 1.0 - exp (-(double) num_samples / (0.003 * p->fs));
    const double a_slow = 1.0 - exp (-(double) num_samples / (0.150 * p->fs));
    const double slow_prev = p->atk_slow;
    p->atk_fast += (rms - p->atk_fast) * a_fast;
    p->atk_slow += (rms - p->atk_slow) * a_slow;
    if (p->atk_refract > 0)
        p->atk_refract -= num_samples;

    /* One pulse per onset EDGE (Schmitt: re-arms only when the fast/slow
       ratio collapses, so a refractory expiring mid-note cannot double). */
    if (! p->atk_armed
        && (p->atk_fast < 1.2 * p->atk_slow || p->atk_fast < AE_GATE_RMS))
        p->atk_armed = true;

    p->onset_pulse = false;
    if (p->atk_armed && p->atk_refract <= 0
        && p->atk_fast > 2.0 * AE_GATE_RMS
        && p->atk_fast > 2.5 * slow_prev)
    {
        p->onset_pulse = true;
        /* A new energy edge is a new event: the detector's octave-
           continuity hysteresis (a raised bar for CHANGING octave
           mid-note) is a claim about the note that just ended, and
           carrying it across the boundary makes the first frames of a
           leap fight the previous note's octave. Clear it; the new note
           earns its own continuity. */
        p->detector.last_best_tau = 0;
        p->atk_armed   = false;
        p->atk_refract = (int) (0.060 * p->fs);
        p->vel_win     = (int) (0.030 * p->fs);
        p->vel_peak    = 0.0;
    }

    if (p->vel_win > 0)
    {
        if (peak > p->vel_peak) p->vel_peak = peak;
        p->vel_win -= num_samples;
        if (p->vel_win <= 0)
        {
            const double vel = vel_from_peak (p, p->vel_peak);
            /* A measured onset is the only thing that RAISES an OBSERVED
               reference, and it does so after being mapped -- so the
               hardest note in the last ~20 s reads 1.0 and sets the bar for
               the rest. A supplied one is never raised; the max() inside
               the map still keeps a louder-than-reference note at unity
               rather than above it. */
            if (p->vel_ref_fixed < 0.0 && p->vel_peak > p->vel_ref)
                p->vel_ref = p->vel_peak;
            atomic_store_explicit (&p->smp_vel_out, (float) vel, memory_order_relaxed);
            /* Refine the strike this window was measuring -- the LEVEL of
               the note now sounding, not the next one's. Only the level:
               the layer (soft vs main) is a different recording and was
               committed at the strike, so a note whose estimate landed the
               wrong side of the soft threshold keeps that timbre. It is
               the one thing measuring cannot fix without delaying the
               strike, which is the latency the feature exists to hide. */
            if (p->smp_vel_fixed < 0.0)
                for (int v = 0; v <= AE_HARM_VOICES; ++v)
                    p->smp[v][p->smp_cur[v]].gain_t = vel;
        }
    }
}

static void attack_process (AeCorrector *p, float *harm_l, float *harm_r,
                            int num_samples)
{

    /* The edge is decided in detect_onset (shared with the sample strike);
       this only decides whether to voice it. */
    if (p->atk_mode != AE_ATK_OFF && harm_l != NULL && p->onset_pulse
        && attack_relevant (p))
        attack_trigger (p);

    if (p->atk_active == 0 || harm_l == NULL)
        return;

    /* The level match ratchets toward the fast follower while the hit
       plays: at trigger time the note is a millisecond old and its RMS
       still climbing, and a pick that tracked only that first reading
       would whisper under every hard note. */
    if (p->atk_fast > p->atk_amp)
        p->atk_amp += (p->atk_fast - p->atk_amp) * 0.5;

    const double g = p->atk_amp * p->atk_jit * p->atk_gain;

    if (p->atk_smp != NULL) /* pick sample, linear-interp at the hit's rate */
    {
        const double scale = g / (p->atk_smp_rms > 1e-6 ? p->atk_smp_rms : 1.0);
        for (int i = 0; i < num_samples; ++i)
        {
            const int    k = (int) p->atk_pos;
            if (k >= p->atk_smp_len - 1)
            {
                p->atk_active = 0;
                p->atk_smp    = NULL;
                break;
            }
            const double f = p->atk_pos - k;
            const double v = ((1.0 - f) * p->atk_smp[k] + f * p->atk_smp[k + 1])
                           / 32768.0 * scale;
            harm_l[i] += (float) v;
            harm_r[i] += (float) v;
            p->atk_pos += p->atk_rate;
        }
    }
    else if (p->atk_active == AE_ATK_NOISE)
    {
        const double r = exp (-2.0 * AE_PI * 1500.0 / p->fs);
        const double scale = g / 0.577; /* uniform noise RMS */
        for (int i = 0; i < num_samples; ++i)
        {
            const double x = (atk_rand01 (p) * 2.0 - 1.0);
            p->atk_hp_y = x - p->atk_hp_x + r * p->atk_hp_y; /* chiff, not thump */
            p->atk_hp_x = x;
            const double v = p->atk_hp_y * p->atk_env * scale;
            harm_l[i] += (float) v;
            harm_r[i] += (float) v;
            p->atk_env *= p->atk_env_a;
        }
        if (p->atk_env < 1e-3)
            p->atk_active = 0;
    }
    else /* AE_ATK_CLICK */
    {
        const double scale = g / 0.707;
        for (int i = 0; i < num_samples; ++i)
        {
            p->atk_ph += p->atk_ph_inc;
            const double v = sin (p->atk_ph) * p->atk_env * scale;
            harm_l[i] += (float) v;
            harm_r[i] += (float) v;
            p->atk_env *= p->atk_env_a;
        }
        if (p->atk_env < 1e-3)
            p->atk_active = 0;
    }
}

void ae_corrector_set_harm_glide_ms (AeCorrector *p, double ms)
{
    p->harm_glide_ms = ms < 0.0 ? 0.0 : (ms > 5000.0 ? 5000.0 : ms);
}

void ae_corrector_set_harm_hold (AeCorrector *p, bool on)
{
    p->harm_hold = on;
}

void ae_corrector_set_harm_sustain (AeCorrector *p, bool on)
{
    p->harm_sustain = on;
    if (! on)
        p->sus_len = 0; /* drop the captured loop; nothing to ring out of */
}

void ae_corrector_set_harm_master (AeCorrector *p, double gain_lin)
{
    p->harm_master = gain_lin < 0.0 ? 0.0 : (gain_lin > 64.0 ? 64.0 : gain_lin);
}

void ae_corrector_set_lead_on (AeCorrector *p, bool on)
{
    p->lead_on = on;
}

static void run_detection (AeCorrector *p)
{
    const long long start = p->in_write - p->frame_size;
    double sum = 0.0;
    for (int k = 0; k < p->frame_size; ++k)
    {
        const long long idx = start + k;
        const float s = (idx >= 0) ? p->in_buf[idx & p->buf_mask] : 0.0f;
        p->frame[k] = s;
        sum += (double) s * s;
    }

    const double rms     = sqrt (sum / p->frame_size);
    const double elapsed = (double) (p->in_write - p->last_detect_at) / p->fs;
    p->last_detect_at = p->in_write;

    const AeYinResult res = ae_yin_process (&p->detector, p->frame, p->frame_size);
    const bool now_voiced = res.voiced && res.frequency_hz > 0.0 && rms > AE_GATE_RMS;

    /* Vocal-tract estimate for LPC vowel mode, on the newest window of the
       same frame the detector just used. Runs whether or not the frame is
       voiced: a consonant has a tract shape too, and that is exactly the
       part a channel vocoder cannot reproduce. A failed fit leaves the
       previous tract standing rather than collapsing the filter. */
    if (p->vowel_mode == AE_VOWEL_MODE_LPC && p->synth_vowel > 0.0)
    {
        const int w = p->frame_size < AE_LPC_WINDOW ? p->frame_size : AE_LPC_WINDOW;
        const float *win = p->frame + (p->frame_size - w);
        double k[AE_LPC_ORDER];
        if (lpc_analyze_frame (win, w, k))
        {
            for (int i = 0; i < AE_LPC_ORDER; ++i)
                p->lpc_k_t[i] = k[i];
            p->lpc_valid = true;
        }
    }

    /* Sung loudness for the synth ghosts (~80 ms smoothing); frozen while
       unvoiced so a release tail holds its level like it holds its pitch. */
    if (now_voiced)
        p->in_level += (rms - p->in_level)
                     * (1.0 - exp (-elapsed / 0.08));

    if (now_voiced)
    {
        const double ref    = p->ref_hz > 0.0 ? p->ref_hz : AE_REFERENCE_C0_HZ;
        const double period = p->period_cents > 0.0 ? p->period_cents : 1200.0;

        const double detected_cents = 1200.0 * log2 (res.frequency_hz / ref);

        /* Split the played pitch into the NOTE and what is being done to
           it. The centre follows at ~180 ms: slower than any bend or
           vibrato, faster than a phrase, so drift is corrected and playing
           is not. A fresh note starts its centre where it was struck
           rather than sliding in from the last one. */
        if (! p->voiced || ! p->primed)
            p->centre_cents = detected_cents;
        else
            p->centre_cents += (detected_cents - p->centre_cents)
                             * (1.0 - exp (-elapsed / 0.180));
        /* Octave re-vote. A guitar with a dominant second harmonic makes
           YIN vote octave-high at the pluck and re-vote the true octave
           mid-note as the uppers decay -- a near-equave step in ONE 5 ms
           hop. No player moves an octave in 5 ms, and even a real leap is
           handled correctly by the same response: take the whole jump into
           the note and leave the expression alone. Tested on the DETECTED
           pitch alone; the target cannot corroborate it any more, because
           the target now follows the centre and the centre is what is
           being corrected here. */
        if (p->voiced && p->primed && p->prev_pair_valid)
        {
            const double dd  = detected_cents - p->prev_det_cents;
            const double add = fabs (dd);
            if (fabs (add - 1200.0) < 60.0 || fabs (add - 1902.0) < 60.0
                || fabs (add - 2400.0) < 60.0)
            {
                p->centre_cents += dd; /* the note moved; follow it at once */
                p->out_cents    += dd; /* and keep the correction continuous */
                p->in_transition = false;
            }
        }
        p->expr_cents = detected_cents - p->centre_cents;

        /* The DEGREE is chosen from the centre, not the instantaneous
           pitch. A vibrato wider than half a step would otherwise flip the
           target back and forth -- in 22-EDO a step is 54.5 cents, which a
           guitarist crosses without trying. */
        const double centre_hz = ref * pow (2.0, p->centre_cents / 1200.0);
        const double steps     = ae_steps_from_ref (centre_hz, p->edo, ref, period);

        /* MIDI Harmony override: while notes are held they ARE the target
           set — middle C (60) pivots to degree 4*edo, one EDO step per
           semitone. No notes held = normal mask behavior. */
        bool      midi_active = false;
        bool      held_mask[AE_MAX_EDO];
        long long held_j[32];
        int       held_n = 0;
        if (p->midi_mode && (p->midi_lo | p->midi_hi) != 0)
        {
            memset (held_mask, 0, sizeof (held_mask));
            for (int n = 0; n < 128 && held_n < 32; ++n)
            {
                const bool on = n < 64 ? ((p->midi_lo >> n) & 1u) != 0
                                       : ((p->midi_hi >> (n - 64)) & 1u) != 0;
                if (! on)
                    continue;
                const long long j = 4LL * p->edo + (n - 60);
                held_j[held_n++] = j;
                held_mask[(int) (((j % p->edo) + p->edo) % p->edo)] = true;
            }
            midi_active = held_n > 0;
        }

        long long cand;
        if (midi_active)
        {
            /* retune to the nearest held note (absolute, not pitch-class) */
            cand = held_j[0];
            double best = fabs (steps - (double) held_j[0]);
            for (int i = 1; i < held_n; ++i)
            {
                const double d = fabs (steps - (double) held_j[i]);
                if (d < best) { best = d; cand = held_j[i]; }
            }
            /* Register fold (default): the held note names a PITCH CLASS;
               the player names the register. Chord voicings sit wherever
               the chord track puts them -- often octaves below a lead line
               -- and unfolded absolute snapping turns that distance into a
               standing transpose: the "incredibly bassy corrected guitar"
               when the nearest held note lives an octave or two down.
               Folding by whole equaves retunes the played note to the held
               class right where it was played. midiOctaves:"held" restores
               the absolute behavior for rigs that use the chord octave to
               place the voice. */
            if (p->midi_fold)
            {
                while (steps - (double) cand > (double) p->edo * 0.5)
                    cand += p->edo;
                while ((double) cand - steps > (double) p->edo * 0.5)
                    cand -= p->edo;
            }
        }
        else
        {
            const AeTuningResult t = ae_quantize_to_edo_scale_ex (centre_hz, p->edo,
                                                                  p->enabled_deg, ref, period);
            cand = t.degree;
        }

        /* Stickiness (hysteresis): stay on the previous target until the
           detected pitch has travelled past the midpoint toward the new
           candidate by an extra `stickiness` fraction of the half-gap.
           Kills degree flicker when the step is smaller than vibrato. */
        if (p->target_valid && cand != p->target_j && p->stickiness > 0.0)
        {
            bool last_ok;
            if (midi_active)
            {
                last_ok = false; /* the old target must still be held */
                for (int i = 0; i < held_n; ++i)
                    if (held_j[i] == p->target_j)
                        last_ok = true;
            }
            else
            {
                const int last_deg = (int) (((p->target_j % p->edo) + p->edo) % p->edo);
                last_ok = p->enabled_deg[last_deg];
            }
            if (last_ok
                && fabs (steps - (double) p->target_j)
                     < (0.5 + 0.5 * p->stickiness) * fabs ((double) (cand - p->target_j)))
                cand = p->target_j;
        }

        const double target_hz    = ae_degree_hz ((long) cand, p->edo, ref, period);
        const double target_cents = 1200.0 * log2 (target_hz / ref);

        /* The static lead transpose (leadShiftSteps), applied AFTER the
           snap. Everything above -- detection, quantize, stickiness,
           tolerance, retune -- ran against the real note; the shift only
           moves what comes out. In whole EDO steps, so +-edo is an exact
           equave and the degree mask never notices. The published target is
           the SHIFTED one: it is what the audience hears, what the pitch
           graph should draw, and what a synth lead must play. */
        const double lead_shift_c = p->edo > 0
            ? (double) p->lead_shift * period / (double) p->edo : 0.0;

        atomic_store_explicit (&p->detected_hz_out, (float) res.frequency_hz, memory_order_relaxed);
        atomic_store_explicit (&p->target_hz_out,
                               (float) (target_hz * pow (2.0, lead_shift_c / 1200.0)),
                               memory_order_relaxed);

        /* On a fresh onset, start from the pitch actually sung so the
           correction glides from there (retune speed) instead of jumping
           from a stale value. */
        if (! p->voiced || ! p->primed)
        {
            p->out_cents     = detected_cents;
            p->target_valid  = false;
            p->in_transition = false;
            p->sustain_s     = 0.0;
        }

        if (! p->target_valid || cand != p->target_j)
        {
            /* New target degree: glide there at the transition speed. */
            p->in_transition = p->target_valid; /* onset itself uses retune speed */
            p->target_j      = cand;
            p->target_valid  = true;
            p->sustain_s     = 0.0;
        }
        else
        {
            p->sustain_s += elapsed;
        }

        /* Tolerance: dead zone around the target where the pitch is left
           alone (preserves vibrato instead of fighting it). Then Amount
           scales whatever correction remains. */
        double eff_cents = target_cents;
        if (fabs (p->centre_cents - target_cents) <= p->tolerance_cents)
            eff_cents = p->centre_cents;
        eff_cents = p->centre_cents + (eff_cents - p->centre_cents) * p->amount;

        /* Retune speed within a note, transition speed between degrees,
           and Humanize relaxes the retune on long-held notes. */
        double tau_ms = p->retune_ms;
        if (p->in_transition)
        {
            tau_ms = p->transition_ms;
            const double step_c = ae_edo_step_cents_ex (p->edo, period);
            if (fabs (p->out_cents - target_cents) < dclamp (0.1 * step_c, 1.0, 5.0))
                p->in_transition = false; /* arrived */
        }
        else if (p->humanize > 0.0)
        {
            const double sustain = p->sustain_s < 1.0 ? p->sustain_s : 1.0;
            tau_ms *= 1.0 + 3.0 * p->humanize * sustain;
        }

        const double tau_sec = tau_ms / 1000.0;
        const double alpha   = (tau_sec <= 0.0) ? 1.0 : (1.0 - exp (-elapsed / tau_sec));
        p->out_cents += (eff_cents - p->out_cents) * alpha;

        /* The shifter takes semitones. Correction alone stays near 1; the
           lead transpose can be octaves, so the clamp is the same +-36 the
           harmony voices get (a safety net, not a musical bound). */
        /* The corrected note centre, with the playing put back on top. */
        const double out_expr = p->out_cents + p->expr_cents * p->expression;
        p->shift_semitones = dclamp ((out_expr + lead_shift_c - detected_cents) / 100.0,
                                     -36.0, 36.0);

        p->primed = true;
        atomic_store_explicit (&p->shift_st_out, (float) p->shift_semitones,
                               memory_order_relaxed);
        {
            /* Decaying extremes: snap outward instantly, relax toward the
               current value with ~1 s of memory. */
            const float sh = (float) p->shift_semitones;
            float mn = atomic_load_explicit (&p->shift_st_min, memory_order_relaxed);
            float mx = atomic_load_explicit (&p->shift_st_max, memory_order_relaxed);
            mn += (sh - mn) * 0.005f;
            mx += (sh - mx) * 0.005f;
            if (sh < mn) mn = sh;
            if (sh > mx) mx = sh;
            atomic_store_explicit (&p->shift_st_min, mn, memory_order_relaxed);
            atomic_store_explicit (&p->shift_st_max, mx, memory_order_relaxed);
        }
        p->prev_det_cents  = detected_cents;
        p->prev_tgt_cents  = target_cents;
        p->prev_pair_valid = true;

        /* ---- harmony voices (Xentar emulation) --------------------------- */
        /* Release slope-freeze. A mute or finger-lift drops the level far
           faster than a string ever decays on its own (a natural ring is a
           few dB/s; a damp is hundreds) while often staying "voiced" for
           tens of milliseconds of bent, dying pitch. A note that was
           stable the whole time it was held must not change at the moment
           it is let go -- so while the level is collapsing, the ghosts
           keep the pitch they had and the rewind ring stops recording. */
        {
            const int rb = (p->rel_pos + AE_REL_RING - 8) % AE_REL_RING;
            if (p->rel_rms[rb] > 0.0f && rms < 0.6 * (double) p->rel_rms[rb])
                goto harmony_done;
        }

        /* HOLD: the ghosts are frozen where they were, so none of the
           retargeting below runs. The LEAD above still tracked normally --
           that is the whole point, you go on singing over the held choir. */
        if (p->harm_hold && p->hold_latched)
            goto harmony_done;

        /* Source = the corrected target degree; voices ride the same glide. */
        bool any_solo = false;
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            if (p->harm[v].solo && p->harm[v].interval != 0 && ! p->harm[v].mute)
                any_solo = true;

        double used_cents[AE_HARM_VOICES];
        int    used_n = 0;

        /* The pitch every ghost is measured FROM. The interval itself comes
           from the snapped degree (cand, below) so it is exact by
           construction -- but an exact interval is only in tune if it is
           stacked on the note the player is actually hearing as the lead.
           With the corrected lead in the mix that is out_cents. With the
           lead muted -- a guitarist harmonising against their own amp --
           the audible lead is the string, so the anchor is what was really
           played. Anchoring to the degree's ideal frequency instead is what
           put the synth ghosts a few cents out: whenever the lead is not
           fully corrected (Amount below 1, inside the tolerance dead zone,
           or still gliding), the lead sits off its ideal degree and a ghost
           pinned to that ideal beats against it. */
        /* The shift is part of the audible lead, so it is part of the
           anchor. With the lead muted the audience hears the raw
           instrument, which the shift does not touch. */
        /* Ghosts anchor to the pitch actually heard as the lead -- which
           now carries the bend, so the harmony bends with it, which is the
           whole point of a harmoniser tracking the player. */
        const double anchor_cents = p->lead_on
            ? p->out_cents + p->expr_cents * p->expression + lead_shift_c
            : detected_cents;

        for (int v = 0; v < AE_HARM_VOICES; ++v)
        {
            const AeHarmVoice *hv = &p->harm[v];
            p->h_active[v] = false;
            int deg_out = AE_HARM_DEG_OFF;

            if (p->harm_on && ae_harm_voice_on (hv))
            {
                /* eff = interval + sign(interval) * extOct * equaveSteps */
                const int eff = hv->interval
                              + (hv->interval > 0 ? 1 : -1) * hv->ext_oct * p->edo;
                /* Intervals stack on the SHIFTED lead degree: an octave-up
                   lead with a "third above" wants the third above the
                   octave-up note, and the mask walk has to happen up there
                   too or the ghost lands a scale interval from a note
                   nobody is hearing. */
                long long gj = cand + p->lead_shift + eff;
                if (p->harm_lock == 1) /* MIDI notes override the mask here too */
                    /* Break a tie AWAY from the lead: up for a ghost above,
                       down for one below, so a third never collapses onto a
                       second or a unison when both neighbours are equally
                       close. A unison ghost has no apartness to preserve
                       and keeps the up-first rule. */
                    gj = ae_walk_to_enabled_dir (gj, p->edo,
                                                 midi_active ? held_mask : p->enabled_deg,
                                                 eff >= 0);

                double ghost_cents = (double) gj * period / (double) p->edo;
                if (p->harm_lock == 2)
                    ghost_cents = ji_snap_cents (ghost_cents, period);

                /* Muted / solo-suppressed voices still report their degree so
                   the UI can dim their ruler tick instead of hiding it. The
                   degree is read off the UNDETUNED pitch: a few cents is a
                   thickening, not a different note, and the ruler should not
                   wobble because of it. */
                deg_out = (int) lround (ghost_cents * (double) p->edo / period);

                /* The detune goes on AFTER the lock, or the lock would snap
                   it straight back out -- and it goes on before the dedupe,
                   so two voices at the same interval detuned apart both
                   sound instead of collapsing into one. This is what makes a
                   unison ghost at, say, -4 cents a usable thickener rather
                   than a phase-coherent double of the lead. */
                ghost_cents += hv->detune_cents;

                if (! hv->mute && ! (any_solo && ! hv->solo))
                {
                    /* Dedupe: voices landing on one pitch must not double up.
                       The deduped voice still reports its degree for the UI. */
                    bool dup = false;
                    for (int u = 0; u < used_n; ++u)
                        if (fabs (ghost_cents - used_cents[u]) < 0.5)
                            dup = true;

                    if (! dup)
                    {
                        used_cents[used_n++] = ghost_cents;

                        /* Interval from the snapped degrees, stacked on the
                           audible lead. Both ghost sources use the same
                           number, so a shifted voice and a synth voice on
                           the same interval land on the same pitch. */
                        const double voice_cents =
                            anchor_cents + (ghost_cents - (target_cents + lead_shift_c));
                        p->h_cents_t[v] = voice_cents;

                        /* Portamento. A voice already sounding slides; one
                           arriving from silence starts on pitch, because
                           sliding in from the last note it happened to sing
                           is a swoop nobody asked for.

                           The slide is LINEAR in cents at a rate fixed when
                           the leg starts, so it arrives in harmGlideMs and
                           then locks on -- not a one-pole toward the target,
                           which is 63% of the way when the time is up and
                           asymptotic ever after. A "leg" is a real
                           retarget: the target moving more than 20 cents in
                           one 5 ms hop, which vibrato and anchor wobble
                           cannot do but a note change always does. Between
                           legs a landed voice tracks its target exactly, so
                           ghosts still ride the lead's vibrato. */
                        if (! p->h_glide_valid[v])
                        {
                            p->h_cents_cur[v]  = voice_cents;
                            p->h_glide_rate[v] = 0.0;
                        }
                        else
                        {
                            const double g_s = p->harm_glide_ms / 1000.0;
                            const double dist = voice_cents - p->h_cents_cur[v];
                            if (g_s <= 0.0)
                                p->h_cents_cur[v] = voice_cents;
                            else
                            {
                                if (fabs (voice_cents - p->h_glide_tgt[v]) > 20.0)
                                    p->h_glide_rate[v] = fabs (dist) / g_s;
                                const double step = p->h_glide_rate[v] * elapsed;
                                if (fabs (dist) <= step || p->h_glide_rate[v] <= 0.0)
                                    p->h_cents_cur[v] = voice_cents; /* landed */
                                else
                                    p->h_cents_cur[v] += dist > 0.0 ? step : -step;
                            }
                        }
                        p->h_glide_tgt[v] = voice_cents;
                        p->h_semitones[v] =
                            dclamp ((p->h_cents_cur[v] - detected_cents) / 100.0,
                                    -36.0, 36.0);
                        p->h_active[v] = true;
                        p->h_glide_valid[v] = true;
                    }
                }
            }
            atomic_store_explicit (&p->h_deg_out[v], deg_out, memory_order_relaxed);
        }
harmony_done: ;
    }
    else
    {
        p->shift_semitones = 0.0; /* identity; the crossfade handles the rest */
        atomic_store_explicit (&p->shift_st_out, 0.0f, memory_order_relaxed);
        p->prev_pair_valid = false;
        p->target_valid    = false;
        p->sustain_s       = 0.0;
        /* Held ghosts keep their gate through the silence -- that is what
           "hold" means. Without the guard the very next unvoiced frame
           would release the choir the performer just parked. */
        if (! (p->harm_hold && p->hold_latched))
            for (int v = 0; v < AE_HARM_VOICES; ++v)
            {
                p->h_active[v] = false;
                atomic_store_explicit (&p->h_deg_out[v], AE_HARM_DEG_OFF, memory_order_relaxed);
            }
    }

    if (now_voiced)
    {
        /* The rewind ring records only while the level is NOT collapsing:
           an entry written during the damp would be exactly the artifact
           the rewind exists to skip. */
        const int rb = (p->rel_pos + AE_REL_RING - 8) % AE_REL_RING;
        const bool releasing = p->rel_rms[rb] > 0.0f
                            && rms < 0.6 * (double) p->rel_rms[rb];
        if (! releasing)
        {
            p->last_voiced_hz = res.frequency_hz;
            p->rel_pos = (uint8_t) ((p->rel_pos + 1) % AE_REL_RING);
            p->rel_det[p->rel_pos] = (float) res.frequency_hz;
            p->rel_rms[p->rel_pos] = (float) rms;
            for (int v = 0; v < AE_HARM_VOICES; ++v)
            {
                p->rel_hc[p->rel_pos][v] = (float) p->h_cents_cur[v];
                p->rel_hs[p->rel_pos][v] = (float) p->h_semitones[v];
            }
        }
    }
    else if (p->voiced)
    {
        /* The note just ended. The final hops tracked the RELEASE -- the
           mute or lift bending the string on its way out -- so rewind the
           ghosts (and the pitch the sustain slice will be cut by) to ~40 ms
           before the drop, and cut the slice from back there too. A held
           choir is exempt: its pitches were chosen at the press and must
           not move. */
        const int back = 8; /* hops of ~5 ms */
        const int idx = (p->rel_pos + AE_REL_RING - back) % AE_REL_RING;
        if (p->rel_det[idx] > 0.0f && ! (p->harm_hold && p->hold_latched))
        {
            p->last_voiced_hz = p->rel_det[idx];
            for (int v = 0; v < AE_HARM_VOICES; ++v)
            {
                p->h_cents_cur[v]  = p->rel_hc[idx][v];
                p->h_cents_t[v]    = p->rel_hc[idx][v];
                p->h_semitones[v]  = p->rel_hs[idx][v];
            }
        }
        capture_sustain (p, p->in_write - (long long) (0.045 * p->fs));
    }
    p->voiced = now_voiced;
    atomic_store_explicit (&p->voiced_out, now_voiced, memory_order_relaxed);

    /* Pitch-trace ring: one point per detection (~200/s), packed into a
       single atomic so a reader can never tear a pair. Unvoiced stores 0 Hz
       detected, which is also the reader's "no note here" marker. The seq
       release-store publishes the slot write above it. */
    {
        union { float f; uint32_t u; } det, tgt;
        det.f = now_voiced ? ae_corrector_detected_hz (p) : 0.0f;
        tgt.f = ae_corrector_target_hz (p);
        const uint32_t s = atomic_load_explicit (&p->trace_seq, memory_order_relaxed);
        atomic_store_explicit (&p->trace_slots[s % AE_TRACE_SLOTS],
                               ((uint64_t) det.u << 32) | tgt.u, memory_order_relaxed);
        atomic_store_explicit (&p->trace_seq, s + 1, memory_order_release);
    }
}

/* Tonality limit for large shifts: frequencies above this are mapped
   non-linearly, which keeps some of the timbre instead of transposing the
   whole spectrum (the poor man's formant preservation — the real thing
   arrives with Signalsmith Stretch 1.3, see shifter.h). Correction-sized
   shifts use a plain linear map, where it buys nothing. */
#define AE_TONALITY_HZ    8000.0
#define AE_TONALITY_MIN_ST 1.0

/* `base_hz` is the detected fundamental, which the library's formant
   analysis wants in order to separate formants from the harmonic series. */
static void set_shift (AeShifter *s, double semitones, double base_hz,
                       bool formant_hold, double formant_st)
{
    ae_shifter_set_semitones (s, semitones,
                              fabs (semitones) >= AE_TONALITY_MIN_ST
                                ? AE_TONALITY_HZ : 0.0);
    if (formant_hold || formant_st != 0.0)
    {
        /* Hold the formants still while the pitch moves, so a transposed
           voice still sounds like the same singer instead of chipmunking --
           and/or shift the tract deliberately: formant_st is a CHARACTER
           control, +up toward a smaller instrument, -down toward a larger
           one, independent of the pitch. */
        ae_shifter_set_formant_semitones (s, formant_st, formant_hold);
        ae_shifter_set_formant_base (s, base_hz);
    }
    else
    {
        /* Fully out of the path (the library skips its envelope machinery
           at multiplier 1 with compensation off): a guitar has no vocal
           tract to preserve, and this removes the formant stage from the
           list of suspects entirely. */
        ae_shifter_set_formant_semitones (s, 0.0, false);
        ae_shifter_set_formant_base (s, 0.0);
    }
}

/* The string-machine ensemble: three delay taps swept by their own pairs of
   LFOs (a slow ~0.6 Hz wander plus a fast ~6 Hz shimmer, the classic
   Solina/Eminent topology), summed back with the dry bus. The taps are
   spread across the stereo field in opposite senses, which is what turns a
   flat rank of saws into a wide, breathing string section. Fractional delay
   is linear-interpolated -- at these sweep depths the interpolation's own
   high-end loss reads as part of the effect.

   Runs on the whole harmony bus rather than per voice: one shared box, like
   the hardware, so a five-note chord swirls together instead of five
   independent choruses fighting each other. */
static void render_ensemble (AeCorrector *p, float *harm_l, float *harm_r,
                             int num_samples)
{
    if (p->ens_buf_l == NULL || p->ens_buf_r == NULL || p->ens_len <= 0)
        return;
    const double depth = dclamp (p->ensemble_depth, 0.0, 1.0);

    /* Tap centres and sweep depths in ms; rates in Hz (slow, fast). */
    static const double c_ms[3]    = { 8.0, 12.0, 16.0 };
    static const double d_ms[3]    = { 2.6,  3.2,  2.2 };
    static const double slow_hz[3] = { 0.31, 0.42, 0.55 };
    static const double fast_hz[3] = { 5.9,  6.4,  5.3 };
    /* Per-tap stereo weights: taps 0 and 2 lean opposite ways, tap 1 sits
       centred, so the sum is wide without losing the mono middle. */
    static const double w_l[3] = { 0.9, 0.7, 0.4 };
    static const double w_r[3] = { 0.4, 0.7, 0.9 };
    /* Normalise the tap sum back to unity, then blend dry against it at
       equal power (they are largely decorrelated, so 1/sqrt2 each). */
#define AE_ENS_WET_NORM (1.0 / (0.9 + 0.7 + 0.4))
#define AE_ENS_BLEND    0.70710678

    const double fs = p->fs;
    for (int t = 0; t < 3; ++t)
    {
        p->ens_lfo[t]     += 2.0 * AE_PI * slow_hz[t] * num_samples / fs;
        p->ens_lfo[t + 3] += 2.0 * AE_PI * fast_hz[t] * num_samples / fs;
        if (p->ens_lfo[t] > 2.0 * AE_PI)     p->ens_lfo[t] -= 2.0 * AE_PI;
        if (p->ens_lfo[t + 3] > 2.0 * AE_PI) p->ens_lfo[t + 3] -= 2.0 * AE_PI;
    }

    /* Delay in samples per tap and SIDE, held for the block (the sweep is
       slow next to a block; stepping per block keeps the sin() count
       trivial). The two sides sweep in OPPOSITE senses: that is what widens
       a centred rank of saws instead of merely thickening it -- with the
       same modulation on both sides the output of a mono bus would stay
       perfectly correlated, i.e. still mono. */
    double delay_l[3], delay_r[3];
    for (int t = 0; t < 3; ++t)
    {
        const double mod = 0.75 * sin (p->ens_lfo[t]) + 0.25 * sin (p->ens_lfo[t + 3]);
        delay_l[t] = dclamp ((c_ms[t] + d_ms[t] * mod) * 0.001 * fs,
                             1.0, (double) (p->ens_len - 2));
        delay_r[t] = dclamp ((c_ms[t] - d_ms[t] * mod) * 0.001 * fs,
                             1.0, (double) (p->ens_len - 2));
    }

    for (int i = 0; i < num_samples; ++i)
    {
        const float dry_l = harm_l[i], dry_r = harm_r[i];
        p->ens_buf_l[p->ens_write & p->ens_mask] = dry_l;
        p->ens_buf_r[p->ens_write & p->ens_mask] = dry_r;

        double wet_l = 0.0, wet_r = 0.0;
        for (int t = 0; t < 3; ++t)
        {
            const double rl = (double) p->ens_write - delay_l[t];
            const double rr = (double) p->ens_write - delay_r[t];
            const int il = (int) floor (rl), ir = (int) floor (rr);
            const double fl = rl - (double) il, fr = rr - (double) ir;
            const float al = p->ens_buf_l[il & p->ens_mask];
            const float bl = p->ens_buf_l[(il + 1) & p->ens_mask];
            const float ar = p->ens_buf_r[ir & p->ens_mask];
            const float br = p->ens_buf_r[(ir + 1) & p->ens_mask];
            /* Tap weights lean the taps across the field; the opposite
               sweeps above are what actually decorrelate the sides. */
            wet_l += w_l[t] * (al + (bl - al) * fl);
            wet_r += w_r[t] * (ar + (br - ar) * fr);
        }
        ++p->ens_write;

        /* Equal-power blend of dry against the (mostly decorrelated) wet
           sum, each normalised by its own weight total: the hardware's
           "ensemble on" is a full-strength blend, not a subtle send, and it
           must not cost level -- these patches are volume-matched to the
           singer like every other one. */
        const double wl = AE_ENS_BLEND * (dry_l + wet_l * AE_ENS_WET_NORM);
        const double wr = AE_ENS_BLEND * (dry_r + wet_r * AE_ENS_WET_NORM);
        /* Depth blends the whole effect against the dry ranks, so a patch's
           swirl can be dialled back without leaving the patch. */
        harm_l[i] = (float) (dry_l + depth * (wl - dry_l));
        harm_r[i] = (float) (dry_r + depth * (wr - dry_r));
    }
}

/* One synth voice's oscillator stack at `hz`, written into `buf`. Shared by
   the harmony ghosts and the lead when it is synth-sourced; `phase`, `lfo`
   and `lp` are that voice's own state, advanced here. */
static void synth_render_voice (AeCorrector *p, const AeSynthPatch *pat,
                                double hz, double *phase, double *lfo, double *lp,
                                float *buf, int n)
{
    for (int i = 0; i < n; ++i)
        buf[i] = 0.0f;

    for (int k = 0; k < pat->n; ++k)
    {
        const AeSynthPartial *pp = &pat->part[k];
        /* Vibrato is stepped once per block: at these rates (< 7 Hz) a block
           is a fraction of a cycle, and a per-sample sin() per partial per
           voice is real money on the audio thread. */
        double cents = pp->detune_cents;
        if (pp->lfo_hz > 0.0f && pp->lfo_cents > 0.0f)
        {
            cents += pp->lfo_cents * sin (lfo[k]);
            lfo[k] += 2.0 * AE_PI * pp->lfo_hz * n / p->fs;
            if (lfo[k] > 2.0 * AE_PI) lfo[k] -= 2.0 * AE_PI;
        }
        const double inc = hz * pp->ratio * pow (2.0, cents / 1200.0) / p->fs;
        if (inc >= 0.45) /* partial would alias; leave it out */
            continue;
        double ph = phase[k];
        if (pp->wave == 1) /* saw */
            for (int i = 0; i < n; ++i)
            {
                ph += inc;
                if (ph >= 1.0) ph -= 1.0;
                buf[i] += (float) (pp->level
                                   * (2.0 * ph - 1.0 - poly_blep (ph, inc)));
            }
        else if (pp->wave == 2) /* square: two band-limited saw edges */
            for (int i = 0; i < n; ++i)
            {
                ph += inc;
                if (ph >= 1.0) ph -= 1.0;
                double ph2 = ph + 0.5;
                if (ph2 >= 1.0) ph2 -= 1.0;
                const double sq = (ph < 0.5 ? 1.0 : -1.0)
                                - poly_blep (ph, inc) + poly_blep (ph2, inc);
                buf[i] += (float) (pp->level * sq);
            }
        else
            for (int i = 0; i < n; ++i)
            {
                ph += inc;
                if (ph >= 1.0) ph -= 1.0;
                buf[i] += (float) (pp->level * sin (2.0 * AE_PI * ph));
            }
        phase[k] = ph;
    }

    if (pat->lp_mult > 0.0f)
    {
        const double cut = dclamp (hz * pat->lp_mult, 500.0, 12000.0);
        const double a = 1.0 - exp (-2.0 * AE_PI * cut / p->fs);
        double s = *lp;
        for (int i = 0; i < n; ++i)
        {
            s += (buf[i] - s) * a;
            buf[i] = (float) s;
        }
        *lp = s;
    }
}

/* The ghost envelope, shared by BOTH harmony sources. Floored at the 5 ms
   click-free ramp, so "0 ms" still means "as fast as is safe" rather than a
   step edge.

   The attack means exactly the same thing either side: how long the ghost
   takes to arrive under the lead. The release does not, and cannot -- a
   synth ghost is an oscillator and keeps sounding after the input stops, so
   its release is a real tail at its last pitch; a shifted ghost is made OF
   the input, so once the input goes quiet there is nothing left to sustain.
   What the release buys on the shifted side is the shape of the leaving:
   the ghost stops chopping off on every consonant and on the moment a
   decaying note drops under the voicing gate. */
static void harm_env_coeffs (const AeCorrector *p, double *atk, double *rel)
{
    const double atk_ms = p->synth_attack_ms  > 5.0 ? p->synth_attack_ms  : 5.0;
    const double rel_ms = p->synth_release_ms > 5.0 ? p->synth_release_ms : 5.0;
    *atk = 1.0 - exp (-1.0 / (atk_ms * 0.001 * p->fs));
    *rel = 1.0 - exp (-1.0 / (rel_ms * 0.001 * p->fs));
}

/* 4b. Synth harmony: oscillator ghosts at the same target degrees the
   shifter voices would sing, each through the shared attack/release
   envelope. Unlike the shifter path there is no voiced crossfade -- a
   released note rings out for the release time at its last pitch, which is
   what makes a pad feel like backing rather than an echo of the singer.
   Allocation-free; voice_buf is reused as the per-voice scratch. */
static void render_synth_harmony (AeCorrector *p, float *harm_l, float *harm_r,
                                  int num_samples)
{
    const AeSynthPatch *pat = &k_synth_patches[p->synth_patch];
    /* Volume-match to the singer: drive the patch to the sung RMS (capped
       against a pathological match on near-silent gated input). */
    const double level = (p->harm_hold && p->hold_latched) ? p->hold_level
                                                           : p->in_level;
    const double match = dclamp (level / synth_patch_rms (pat), 0.0, 4.0);
    double atk_a, rel_a;
    harm_env_coeffs (p, &atk_a, &rel_a);
    /* Note-to-note glide, stepped once per block (~25 ms time constant). */
    const double glide_a = 1.0 - exp (-num_samples / (0.025 * p->fs));

    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        if (ae_corrector_voice_source (p, v) != AE_HARM_SRC_SYNTH)
            continue; /* this voice is shifted; the shifter pass renders it */
        p->h_fed[v] = false; /* its shifter idles; refill on switch back */

        const bool gate = p->h_active[v];
        double env = p->s_env[v];
        if (! gate && env < 1e-4)
        {
            p->s_env[v] = 0.0;
            p->h_glide_valid[v] = false; /* truly silent: next note starts on pitch */
            continue;
        }

        /* A fresh attack starts on pitch; a sounding voice glides. */
        if (gate && env < 1e-3)
            p->s_cents[v] = p->h_cents_cur[v];
        else
            p->s_cents[v] += (p->h_cents_cur[v] - p->s_cents[v]) * glide_a;
        const double hz = p->ref_hz * pow (2.0, p->s_cents[v] / 1200.0);

        float *buf = p->voice_buf;
        synth_render_voice (p, pat, hz, p->s_phase[v], p->s_lfo[v], &p->s_lp[v],
                            buf, num_samples);

        const double gl = p->h_gl[v], gr = p->h_gr[v];
        const double want = gate ? 1.0 : 0.0;
        const double a = gate ? atk_a : rel_a;
        for (int i = 0; i < num_samples; ++i)
        {
            env += (want - env) * a;
            const double s = buf[i] * env * match;
            harm_l[i] += (float) (gl * s);
            harm_r[i] += (float) (gr * s);
        }
        p->s_env[v] = env;
    }

    /* Vowel transfer before the ensemble: the formants belong to the notes,
       and smearing them afterwards is what the ensemble is for. */
    if (p->synth_vowel > 0.0)
    {
        if (p->vowel_mode == AE_VOWEL_MODE_LPC)
        {
            lpc_apply (p, harm_l, num_samples, 0, p->lpc_res, p->synth_vowel);
            lpc_apply (p, harm_r, num_samples, 1, p->lpc_res, p->synth_vowel);
        }
        else
        {
            voc_apply (p, harm_l, num_samples, 0, p->synth_vowel);
            voc_apply (p, harm_r, num_samples, 1, p->synth_vowel);
        }
    }

    /* The drone: one absolute-pitch voice, deliberately AFTER the vowel
       stage (a drone has no mouth to follow -- the vocoder's gating would
       mute it between phrases, the exact opposite of a drone) and BEFORE
       the ensemble, so it swirls with the section like any other rank. */
    {
        const bool gate = p->harm_on && p->drone_on;
        double env = p->drone_env;
        if (gate || env >= 1e-4)
        {
            const double cents_t = (double) p->drone_j * (p->period_cents > 0.0
                                       ? p->period_cents : 1200.0)
                                 / (double) (p->edo > 0 ? p->edo : AE_MIN_EDO);
            if (gate && env < 1e-3)
                p->drone_cents = cents_t; /* a fresh drone starts on pitch */
            else
                p->drone_cents += (cents_t - p->drone_cents) * glide_a;
            const double hz = (p->ref_hz > 0.0 ? p->ref_hz : AE_REFERENCE_C0_HZ)
                            * pow (2.0, p->drone_cents / 1200.0);
            float *buf = p->voice_buf;
            synth_render_voice (p, pat, hz, p->drone_phase, p->drone_lfo,
                                &p->drone_lp, buf, num_samples);
            const double want = gate ? 1.0 : 0.0;
            const double a = gate ? atk_a : rel_a;
            for (int i = 0; i < num_samples; ++i)
            {
                env += (want - env) * a;
                const double s = buf[i] * env * match * 0.70710678; /* centre */
                harm_l[i] += (float) s;
                harm_r[i] += (float) s;
            }
            p->drone_env = env < 1e-4 && ! gate ? 0.0 : env;
        }
    }

    if (pat->ensemble && p->ensemble_depth > 0.0)
        render_ensemble (p, harm_l, harm_r, num_samples);
}

/* The layer law, shared by every sample voice: 0 = the shifted rendering
   alone, 1 = the sample alone, and 0.5 = BOTH AT UNITY -- a plateau at
   centre with an equal-power taper either side, rather than the textbook
   crossfade that would drop both to 0.707 in the middle. One number means
   the same thing here as in the rig's other layers. */
static void sample_layer_gains (double mix, double *g_shift, double *g_smp)
{
    const double a = mix <= 0.5 ? 1.0 : 2.0 * (1.0 - mix);
    const double b = mix >= 0.5 ? 1.0 : 2.0 * mix;
    *g_shift = sin (a * (AE_PI / 2.0));
    *g_smp   = sin (b * (AE_PI / 2.0));
}

/* 4c. Sample harmony. A ghost is continuous; a sample is struck -- so a
   sample voice is struck at the LEAD'S ONSET (the same edge the attack
   sound uses, fired before the detector has a pitch) and then re-pitched,
   never re-struck, for as long as that note lasts. Xentar's legato
   discipline, ported: repitch the live voice, and retrigger by starting a
   fresh slot across a short fade rather than cutting a sounding one. */
static void render_sample_harmony (AeCorrector *p, float *harm_l, float *harm_r,
                                   int num_samples)
{
    if (atomic_load_explicit (&p->smp_live, memory_order_acquire) < 0)
        return;

    double atk_a, rel_a;
    harm_env_coeffs (p, &atk_a, &rel_a);
    double g_shift, g_smp;
    sample_layer_gains (p->smp_mix, &g_shift, &g_smp);

    /* The strike level. Measuring takes 30 ms and the strike cannot wait
       for it, so a struck voice takes the fast follower's reading NOW and
       the window's verdict corrects it 30 ms later (see detect_onset).
       Reading smp_vel_out here instead would strike every note at the
       PREVIOUS note's level, which is exactly wrong the moment the
       dynamics change. */
    const double vel = p->smp_vel_fixed >= 0.0
        ? p->smp_vel_fixed
        : vel_from_peak (p, p->atk_fast * AE_SQRT2); /* RMS -> peak estimate */
    const double period = p->period_cents > 0.0 ? p->period_cents : 1200.0;
    const double ref    = p->ref_hz > 0.0 ? p->ref_hz : AE_REFERENCE_C0_HZ;

    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        if (ae_corrector_voice_source (p, v) != AE_HARM_SRC_SAMPLE)
            continue;
        p->h_fed[v] = false;

        const bool gate = p->h_active[v];
        const double hz = ref * pow (2.0, p->h_cents_cur[v] / 1200.0);
        (void) period;

        /* Strike on the note EDGE -- but a pitched sample cannot be struck
           before there is a pitch to strike at, and the voicing gate lags
           the energy edge by several detection hops. So the onset ARMS the
           strike and it fires at the first moment a pitch exists: instantly
           when the voice already has a glide position (the common case
           inside a phrase, so the strike really is at the onset), and on
           the first lock for a cold note. The attack sound covers the
           interval either way -- that is what it is for. Waiting is capped
           so a pending strike cannot fire into the next phrase. */
        if (p->onset_pulse)
        {
            p->smp_pending[v] = true;
            p->smp_wait[v] = (int) (0.20 * p->fs);
        }
        if (p->smp_pending[v])
        {
            p->smp_wait[v] -= num_samples;
            if (p->smp_wait[v] <= 0)
                p->smp_pending[v] = false;
            else if ((gate || p->h_glide_valid[v]) && hz > 0.0)
            {
                sample_strike (p, v, hz, vel);
                p->smp_pending[v] = false;
            }
        }
        sample_repitch (p, v, hz); /* repitch, not restrike */

        double env = p->smp_env[v];
        if (! gate && env < 1e-4 && ! p->smp_pending[v]
            && sample_voice_idle (p, v))
        {
            p->smp_env[v] = 0.0;
            continue;
        }

        const double gl = p->h_gl[v], gr = p->h_gr[v];
        const double want = gate ? 1.0 : 0.0;
        const double a = gate ? atk_a : rel_a;
        /* 6 ms retirement fade on the slots being replaced -- crossfade past
           a sounding voice, never interrupt it. */
        const double fade_step = 1.0 / (0.006 * p->fs);
        for (int i = 0; i < num_samples; ++i)
        {
            env += (want - env) * a;
            const double x = sample_mix_slots (p, v, fade_step, rel_a);
            const double sv = x * env * g_smp;
            harm_l[i] += (float) (gl * sv);
            harm_r[i] += (float) (gr * sv);
        }
        p->smp_env[v] = env;
    }
}

/* The harmony-bus master: the LAST thing on the ghost bus and the only
   thing on all of it. Smoothed over ~5 ms so a controller sweeping the
   fader cannot zipper, and deliberately applied after the IR and the tilt
   so pulling the bus down takes its reverb tail with it. */
static void apply_harm_master (AeCorrector *p, float *harm_l, float *harm_r,
                               int num_samples)
{
    const double target = p->harm_master;
    double g = p->harm_master_cur;
    if (g == target)
    {
        if (g == 1.0)
            return;
        for (int i = 0; i < num_samples; ++i)
        {
            harm_l[i] = (float) (harm_l[i] * g);
            harm_r[i] = (float) (harm_r[i] * g);
        }
        return;
    }
    const double a = 1.0 - exp (-1.0 / (0.005 * p->fs));
    for (int i = 0; i < num_samples; ++i)
    {
        g += (target - g) * a;
        harm_l[i] = (float) (harm_l[i] * g);
        harm_r[i] = (float) (harm_r[i] * g);
    }
    p->harm_master_cur = fabs (target - g) < 1e-6 ? target : g;
}

static void process_chunk (AeCorrector *p, float *mono, float *harm_l,
                           float *harm_r, float *wet_out, int num_samples)
{
    /* 1. Take in the block: keep a contiguous copy (mono is written in place
       below) and push it into the ring, which feeds detection and the dry
       path. */
    /* HOLD edges. Engaging takes ONE snapshot -- the sustain loop and the
       level the choir will hold at -- rather than chasing the input, so a
       held chord stays the chord that was sung and does not follow whatever
       is sung over it. If the loop was captured at the end of an earlier
       note, that one is kept; a hold pressed mid-note captures now. */
    if (p->harm_hold && ! p->hold_latched)
    {
        if (p->voiced)
            capture_sustain (p, p->in_write);
        p->hold_level   = p->in_level;
        p->hold_latched = true;
    }
    else if (! p->harm_hold && p->hold_latched)
    {
        p->hold_latched = false;
    }

    memcpy (p->in_block, mono, (size_t) num_samples * sizeof (float));
    for (int i = 0; i < num_samples; ++i)
    {
        p->in_buf[p->in_write & p->buf_mask] = mono[i];
        ++p->in_write;

        if (p->in_write - p->last_detect_at >= p->hop && p->in_write >= p->frame_size)
            run_detection (p);
    }

    /* 1a. The note edge and its strike level, before anything that needs
       either -- the attack sound and the sample strike both fire ahead of
       the detector having a pitch. */
    detect_onset (p, num_samples);

    /* 1b. Vowel analysis runs on the input whatever the sources are: it is
       one pass, and having the envelopes already tracking means switching
       the transfer on mid-phrase lands on the vowel being sung rather than
       ramping up from silence. */
    if (p->synth_vowel > 0.0)
    {
        if (p->vowel_mode == AE_VOWEL_MODE_LPC)
        {
            /* Slew the tract toward the newest estimate (~15 ms), in
               reflection-coefficient space so every intermediate set is
               still a stable filter, and whiten this block for its
               consonant content. */
            const double a = 1.0 - exp (-num_samples / (0.015 * p->fs));
            for (int i = 0; i < AE_LPC_ORDER; ++i)
                p->lpc_k[i] += (p->lpc_k_t[i] - p->lpc_k[i]) * a;
            if (p->lpc_valid && p->lpc_res != NULL)
                lpc_residual (p, p->in_block, p->lpc_res, num_samples);
        }
        else
            voc_analyze (p, p->in_block, num_samples);
    }

    const bool lead_synth  = p->lead_source == AE_HARM_SRC_SYNTH;
    const bool lead_sample = p->lead_source == AE_HARM_SRC_SAMPLE;

    /* 2. Shift. The detection above is centred about half an analysis frame
       behind the newest input, which is close to where the shifter's own
       processing time sits (its input latency), so the ratio lands on the
       audio it was measured from. A synth lead skips the shifter entirely
       and plays the corrected pitch instead. */
    const double base_hz =
        atomic_load_explicit (&p->detected_hz_out, memory_order_relaxed);
    if (lead_sample)
    {
        /* A sampled LEAD: struck at the onset, re-pitched after. Same
           discipline as a sample ghost, aimed at the corrected target. */
        const double hz =
            (double) atomic_load_explicit (&p->target_hz_out, memory_order_relaxed);
        const double vel = p->smp_vel_fixed >= 0.0 ? p->smp_vel_fixed
            : vel_from_peak (p, p->atk_fast * AE_SQRT2);
        const int L = AE_HARM_VOICES;
        if (p->onset_pulse)
        {
            p->smp_pending[L] = true;
            p->smp_wait[L] = (int) (0.20 * p->fs);
        }
        if (p->smp_pending[L])
        {
            p->smp_wait[L] -= num_samples;
            if (p->smp_wait[L] <= 0)
                p->smp_pending[L] = false;
            else if (hz > 0.0)
            {
                sample_strike (p, L, hz, vel);
                p->smp_pending[L] = false;
            }
        }
        sample_repitch (p, L, hz);
        const double fade_step = 1.0 / (0.006 * p->fs);
        /* The LEAD's ceiling is its own release, not the harmony's. */
        const double lr = p->lead_release_ms > 5.0 ? p->lead_release_ms : 5.0;
        const double l_rel = 1.0 - exp (-1.0 / (lr * 0.001 * p->fs));
        for (int i = 0; i < num_samples; ++i)
            p->wet_buf[i] = (float) sample_mix_slots (p, L, fade_step, l_rel);
    }
    else if (lead_synth)
    {
        const AeSynthPatch *pat = &k_synth_patches[p->synth_patch];
        const double hz =
            (double) atomic_load_explicit (&p->target_hz_out, memory_order_relaxed);
        if (hz > 0.0)
            synth_render_voice (p, pat, hz, p->lead_phase, p->lead_lfo, &p->lead_lp,
                                p->wet_buf, num_samples);
        else
            for (int i = 0; i < num_samples; ++i)
                p->wet_buf[i] = 0.0f;
        /* Volume-matched like the ghosts, and given the singer's vowel when
           the transfer is up -- a synth lead that tracks the mouth is the
           point of pointing it at the lead. */
        const double match = dclamp (p->in_level / synth_patch_rms (pat), 0.0, 4.0);
        for (int i = 0; i < num_samples; ++i)
            p->wet_buf[i] = (float) (p->wet_buf[i] * match);
        if (p->synth_vowel > 0.0)
        {
            if (p->vowel_mode == AE_VOWEL_MODE_LPC)
                lpc_apply (p, p->wet_buf, num_samples, 2, p->lpc_res, p->synth_vowel);
            else
                voc_apply (p, p->wet_buf, num_samples, 2, p->synth_vowel);
        }
    }
    else
    {
        set_shift (p->shifter, p->shift_semitones, base_hz,
                   p->formant_hold, p->formant_st);
        ae_shifter_process (p->shifter, p->in_block, p->wet_buf, num_samples);
    }

    /* 3. Deliver the corrected voice against the latency-matched dry path.
       A synth lead has no dry component: its "unvoiced" is silence, not the
       raw microphone, which is what makes it a synth lead rather than a
       passthrough with occasional synth. */
    const long long block_start = p->in_write - num_samples;
    const double gain_alpha = 1.0 - exp (-1.0 / (0.005 * p->fs)); /* ~5 ms crossfade */
    double v_gain = p->v_gain;

    /* The lead's OWN envelope, floored at the same 5 ms click guard the
       harmony uses. Which gain it replaces depends on whether this lead has
       a dry half:

       - Synth and sample leads have none (their "unvoiced" is silence, not
         the raw microphone). The envelope IS their gate, so the release is
         a real tail -- and on a let-ringing sample it is a CEILING over the
         recording's natural decay, closing a note that would otherwise ring
         past where the player wanted it.
       - A shifted lead crossfades to the dry input when the voicing drops,
         so its wet must still follow v_gain or a consonant would sound
         twice. The attack shapes its arrival; the release is inert by
         construction, because a shifted lead is made OF the input and there
         is nothing left to sustain once the input stops. */
    const double la_ms = p->lead_attack_ms  > 5.0 ? p->lead_attack_ms  : 5.0;
    const double lr_ms = p->lead_release_ms > 5.0 ? p->lead_release_ms : 5.0;
    const double l_atk = 1.0 - exp (-1.0 / (la_ms * 0.001 * p->fs));
    const double l_rel = 1.0 - exp (-1.0 / (lr_ms * 0.001 * p->fs));
    const bool   no_dry = lead_synth || lead_sample;
    double l_env = p->lead_env;

    for (int i = 0; i < num_samples; ++i)
    {
        const double target = p->voiced ? 1.0 : 0.0;
        v_gain += (target - v_gain) * gain_alpha;
        l_env  += (target - l_env) * (p->voiced ? l_atk : l_rel);

        const long long t_out = block_start + i - p->latency;
        const float dry = (no_dry || t_out < 0)
                              ? 0.0f : p->in_buf[t_out & p->buf_mask];
        const double wet_g = no_dry ? l_env : v_gain * l_env;
        const double wet_only = wet_g * p->wet_buf[i];
        if (wet_out != NULL)
            wet_out[i] = (float) wet_only;
        mono[i] = (float) (wet_only + (1.0 - v_gain) * dry);
    }
    p->v_gain = v_gain;
    p->lead_env = l_env;

    /* The LEAD IR point: on the finished corrected voice. Zero added
       latency by construction (direct first partition) -- this is the live
       monitored path the whole scheme exists for. */
    if (p->ir_lead != NULL)
        irc_point_process (p->ir_lead, mono, mono, num_samples);

    if (harm_l == NULL)
    {
        attack_process (p, NULL, NULL, num_samples); /* keep the followers warm */
        return;
    }

    for (int i = 0; i < num_samples; ++i)
    {
        harm_l[i] = 0.0f;
        harm_r[i] = 0.0f;
    }

    /* 4a. Synth-sourced harmony voices (with the vowel and ensemble stages),
       then 4b below adds any voices still on the shifter. */
    bool any_synth = false, any_shift = false;
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        const int src = ae_corrector_voice_source (p, v);
        if (src == AE_HARM_SRC_SYNTH)
            any_synth = true;
        else if (src == AE_HARM_SRC_SAMPLE)
        {
            /* A sample voice still drives its shifter whenever the blend
               asks for any of it -- "sample" is a LAYER, not a swap. */
            if (p->smp_mix < 1.0) any_shift = true;
        }
        else
            any_shift = true;
    }
    if (any_synth || p->drone_on || p->drone_env >= 1e-4)
        render_synth_harmony (p, harm_l, harm_r, num_samples);
    render_sample_harmony (p, harm_l, harm_r, num_samples);
    if (! any_shift)
    {
        attack_process (p, harm_l, harm_r, num_samples);
        /* The harmony IR point (post-ensemble), then the tilt: the tilt
           stays the performer's final tone trim over whatever space the IR
           imposes. */
        if (p->ir_harm[0] != NULL)
        {
            irc_point_process (p->ir_harm[0], harm_l, harm_l, num_samples);
            irc_point_process (p->ir_harm[1], harm_r, harm_r, num_samples);
        }
        if (p->harm_tilt_db != 0.0)
            render_tilt (p, harm_l, harm_r, num_samples);
        apply_harm_master (p, harm_l, harm_r, num_samples);
        return;
    }

    /* 4. Harmony voices: one shifter each, mixed through their own smoothed
       gain so mute/solo and voice changes can't click. A configured voice is
       fed even while muted, so unmuting is instant rather than costing a
       shifter's worth of latency to refill. */
    double h_atk, h_rel;
    harm_env_coeffs (p, &h_atk, &h_rel);

    /* What the harmony shifters read. Normally the live input, same as the
       lead. While a ghost is ringing on past its source -- a HOLD, or a
       release with sustain enabled -- they read the captured loop instead,
       so there is something to transpose. The LEAD never reads it: the lead
       is the performer, and a lead that sustained itself would be a
       different instrument. */
    const float *harm_in = p->in_block;
    {
        const bool held = p->harm_hold && p->hold_latched;
        const bool want_loop = p->sus_len > 0
                             && (held || (p->harm_sustain && ! p->voiced));
        const double want = want_loop ? 1.0 : 0.0;
        if (want > 0.0 || p->sus_mix > 1e-4)
        {
            const double a = 1.0 - exp (-1.0 / (0.005 * p->fs));
            double m = p->sus_mix;
            for (int i = 0; i < num_samples; ++i)
            {
                m += (want - m) * a;
                float loop = 0.0f;
                if (p->sus_len > 0)
                {
                    loop = p->sus_buf[p->sus_read];
                    if (++p->sus_read >= p->sus_len)
                        p->sus_read = 0;
                }
                p->sus_block[i] = (float) ((1.0 - m) * p->in_block[i] + m * loop);
            }
            p->sus_mix = m;
            harm_in = p->sus_block;
        }
        else
            p->sus_mix = 0.0;
    }

    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        const int  vsrc = ae_corrector_voice_source (p, v);
        const bool configured = p->harm_on && ae_harm_voice_on (&p->harm[v])
                              && p->h_shifter[v] != NULL
                              && (vsrc == AE_HARM_SRC_VOICE
                                  || (vsrc == AE_HARM_SRC_SAMPLE && p->smp_mix < 1.0));
        if (! configured)
        {
            p->h_fed[v] = false; /* history is stale; reset before reuse */
            p->h_mix[v] = 0.0;
            p->h_glide_valid[v] = false;
            continue;
        }

        if (! p->h_fed[v])
        {
            ae_shifter_reset (p->h_shifter[v]);
            p->h_fed[v] = true;
            p->h_mix[v] = 0.0; /* fade in from silence */
        }

        set_shift (p->h_shifter[v], p->h_semitones[v], base_hz,
                   p->formant_hold, p->formant_st);
        ae_shifter_process (p->h_shifter[v], harm_in, p->voice_buf, num_samples);

        /* Same envelope the synth ghosts get. It used to be a fixed 5 ms
           ramp multiplied by the LEAD's voiced crossfade (v_gain) -- and
           that second factor was the real reason a release could not be
           heard here, because it cut the ghost within 5 ms of the lead
           going unvoiced whatever the envelope was doing. It was also
           redundant: h_active[v] already falls with voicing, so the gate
           is not lost by dropping it. */
        const bool   gate = p->h_active[v];
        const double want = gate ? 1.0 : 0.0;
        const double a    = gate ? h_atk : h_rel;
        const double gl = p->h_gl[v], gr = p->h_gr[v];
        double lay_shift = 1.0, lay_smp;
        if (vsrc == AE_HARM_SRC_SAMPLE)
            sample_layer_gains (p->smp_mix, &lay_shift, &lay_smp);
        double mix = p->h_mix[v];
        for (int i = 0; i < num_samples; ++i)
        {
            mix += (want - mix) * a;
            const double s = p->voice_buf[i] * mix * lay_shift;
            harm_l[i] += (float) (gl * s);
            harm_r[i] += (float) (gr * s);
        }
        p->h_mix[v] = mix;
        if (! gate && mix < 1e-4)
            p->h_glide_valid[v] = false; /* the release finished */
    }

    attack_process (p, harm_l, harm_r, num_samples);

    /* Harmony IR before the tilt, same order as the synth-only path. */
    if (p->ir_harm[0] != NULL)
    {
        irc_point_process (p->ir_harm[0], harm_l, harm_l, num_samples);
        irc_point_process (p->ir_harm[1], harm_r, harm_r, num_samples);
    }
    if (p->harm_tilt_db != 0.0)
        render_tilt (p, harm_l, harm_r, num_samples);
    apply_harm_master (p, harm_l, harm_r, num_samples);
}

void ae_corrector_process (AeCorrector *p, float *mono, float *harm_l, float *harm_r,
                       int num_samples)
{
    if (p->in_buf == NULL || num_samples <= 0)
        return;
    if (harm_l == NULL || harm_r == NULL)
        harm_l = harm_r = NULL;

    /* Sub-chunk so a block larger than the prepared maximum can never
       overflow the ring buffers. */
    int done = 0;
    while (done < num_samples)
    {
        int m = num_samples - done;
        if (m > p->max_block) m = p->max_block;
        process_chunk (p, mono + done,
                       harm_l != NULL ? harm_l + done : NULL,
                       harm_r != NULL ? harm_r + done : NULL,
                       p->lead_wet != NULL && done + m <= p->max_block
                           ? p->lead_wet + done : NULL, m);
        done += m;
    }
}
