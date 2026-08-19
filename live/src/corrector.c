#include "corrector.h"
#include "attack_picks.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AE_PI       3.14159265358979323846
#define AE_SQRT2    1.41421356237309504880
#define AE_MIN_FREQ 65.0    /* default lowest detectable pitch (Hz) */
#define AE_MAX_FREQ 1600.0  /* default highest detectable pitch (Hz) */
#define AE_GATE_RMS 0.0015  /* ~ -56 dBFS default noise gate; gateDb
                               overrides per rig (p->gate_rms) -- no fixed
                               floor fits every pickup's level */
#define GATE(p) ((p)->gate_rms > 0.0 ? (p)->gate_rms : AE_GATE_RMS)

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
    p->poly      = (quality & AE_SHIFT_QUALITY_POLY_FLAG) != 0;
    quality     &= ~AE_SHIFT_QUALITY_POLY_FLAG;

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
    if (p->poly)
        p->block_samples *= 2; /* chords want the longer analysis window */
    ae_polyf0_free (&p->polyf0);
    if (p->poly)
    {
        /* The tracker's range rides the detection-range controls, exactly
           as the mono detector's does -- the user's placement. */
        ae_polyf0_prepare (&p->polyf0, p->fs, min_hz,
                           max_hz < 1200.0 ? max_hz : 1200.0);
        for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
        {
            p->poly_prev_id[k]  = -1;
            p->poly_base_raw[k] = 0.0;
            p->poly_refired[k]  = false;
        }
        p->poly_fill     = 0;
        p->poly_burst    = 0;
        p->poly_restrike = 0;
        p->poly_fb       = 0;
        p->poly_fb_due   = false;
        for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
            p->poly_dev_ema[k] = 0.0;
        if (p->poly_cap <= 0 || p->poly_cap > AE_POLY_MAX_NOTES)
            p->poly_cap = AE_POLY_MAX_NOTES;
    }
    atomic_store_explicit (&p->poly_active_out, 0, memory_order_relaxed);
    for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
        atomic_store_explicit (&p->poly_note_out[k], 0, memory_order_relaxed);
    p->steel_deg = AE_HARM_DEG_OFF; /* calloc's 0 is a real degree */
    p->steel_env = 0.0;

    ae_corrector_free_shifters (p);
    p->shifter = ae_shifter_create (p->fs, p->block_samples);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        p->h_shifter[v] = ae_shifter_create (p->fs, p->block_samples);
    if (p->poly)
        /* the MEL remodeller's bank: unison resynth + two footages.
           Poly only -- the layer is the chord sampler's companion. Idle
           layers cost nothing (skipped below zero gain). */
        for (int l = 0; l < 3; ++l)
            p->mel_shifter[l] = ae_shifter_create (p->fs, p->block_samples);

    p->latency = p->shifter != NULL ? ae_shifter_latency (p->shifter)
                                    : p->block_samples;

    /* The poly tracker's window outgrows the YIN frame (4096+ against two
       periods of min_hz), and it borrows p->frame as its staging scratch --
       so the ALLOCATIONS follow the larger of the two, while frame_size
       itself keeps its YIN meaning. */
    const int frame_alloc =
        p->poly && p->polyf0.win_size > p->frame_size ? p->polyf0.win_size
                                                      : p->frame_size;
    const int need = p->latency + 2 * p->max_block + frame_alloc + 16;
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
    p->frame     = calloc ((size_t) frame_alloc, sizeof (float));
    p->in_block  = calloc ((size_t) p->max_block, sizeof (float));
    p->wet_buf   = calloc ((size_t) p->max_block, sizeof (float));
    p->voice_buf = calloc ((size_t) p->max_block, sizeof (float));
    free (p->lead_wet);
    p->lead_wet  = calloc ((size_t) p->max_block, sizeof (float));
    free (p->mel_sum);
    p->mel_sum   = calloc ((size_t) p->max_block, sizeof (float));
    free (p->steel_buf);
    p->steel_buf = calloc ((size_t) p->max_block, sizeof (float));
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
    p->att_prev_valid = false;
    p->attack_hold     = 0;
    p->att_hold_recent = 0;
    p->att_hold_j      = 0;
    p->smp_guard          = 0;
    p->smp_lead_deg_valid = false;
    p->att_seq_seen       = p->att_seq;
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
    if (p->smp_register == 0) p->smp_register = AE_SMP_OCTAVE_AUTO;
    p->smp_reg_ratio = 1.0; /* resolved when a bank loads / the key writes */
    /* Voices default to "follow the global source" -- the documented
       meaning of AE_HARM_SRC_DEFAULT. A zeroed struct would otherwise read
       as an explicit per-voice AE_HARM_SRC_VOICE override and silently
       pin every voice to the shifter. */
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        p->h_source[v] = AE_HARM_SRC_DEFAULT;
    p->smp_gain_a = 1.0 - exp (-1.0 / (0.015 * p->fs));
    p->onset_pulse = false;
    p->vel_win = 0; p->vel_peak = 0.0;
    p->vel_sq = 0.0; p->vel_n = 0;
    p->vel_ema_db = -999.0; /* no verdict yet */
    p->smp_match  = 0.0;    /* the PRIMITIVE keeps the map's semantics; the
                               rig-level default of 1 (parity) is the config
                               layer's (config_defaults), so a direct
                               consumer of this struct is never surprised */
    p->smp_tilt   = 0.0;    /* same split: the rig's +3 dB/oct lives in
                               config_defaults */
    p->smp_level  = 1.0;
    p->smp_tone_db = 0.0;
    p->smp_tone_db_cur = 1e9; /* force a gain rebuild on first use */
    memset (p->smp_tone_lp, 0, sizeof (p->smp_tone_lp));
    p->smp_dry_g  = 0.0;
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
    atomic_store_explicit (&p->lead_deg_out, AE_HARM_DEG_OFF, memory_order_relaxed);
    atomic_store_explicit (&p->env_out, 0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->follow_level_in, 1.0f, memory_order_relaxed);
    p->follow_gain_cur = 1.0;
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
    for (int l = 0; l < 3; ++l)
    {
        ae_shifter_destroy (p->mel_shifter[l]);
        p->mel_shifter[l] = NULL;
    }
}

void ae_corrector_free (AeCorrector *p)
{
    ae_polyf0_free (&p->polyf0);
    free (p->in_buf);
    free (p->frame);
    free (p->in_block);
    free (p->wet_buf);
    free (p->voice_buf);
    free (p->lead_wet);
    p->lead_wet = NULL;
    p->in_buf = p->frame = p->in_block = p->wet_buf = p->voice_buf = NULL;
    free (p->mel_sum);
    p->mel_sum = NULL;
    free (p->steel_buf);
    p->steel_buf = NULL;
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
        for (int v = 0; v <= AE_HARM_VOICES + 1; ++v)
            for (int k = 0; k < AE_SMP_SLOTS; ++k)
                if (k != p->smp_cur[v] && p->smp[v][k].rec != NULL)
                    p->smp[v][k].retiring = true;
    p->smp_ring = ring;
}

void ae_corrector_set_poly_notes (AeCorrector *p, int cap)
{
    /* <1 = unset/uncapped: a zeroed config must not mean "monophonic". */
    p->poly_cap = cap < 1 ? AE_POLY_MAX_NOTES
                          : (cap > AE_POLY_MAX_NOTES ? AE_POLY_MAX_NOTES : cap);
}

void ae_corrector_set_follow (AeCorrector *p, double amt)
{
    p->follow_amt = dclamp (amt, 0.0, 1.0);
}

/* Supply the velocity reference instead of observing one. `ref_lin` is a
   linear peak; negative means observe. Applied on the next block, so a
   host may re-assert it per phrase without clicking anything: the
   reference scales velocities, it is not in the audio path. */
void ae_corrector_set_sample_match (AeCorrector *p, double m)
{
    p->smp_match = m < 0.0 ? 0.0 : (m > 1.0 ? 1.0 : m);
}

void ae_corrector_set_strike_tilt (AeCorrector *p, double db_oct)
{
    /* +-12, doubled from +-6 (field: "Tilt up to the max but high notes
       are much too quiet through the samples") -- a guitar's thin strings
       plus a pickup's rolloff can fall faster than 6 dB/oct above the
       pivot, and a range that cannot reach the correction is a range
       that lies about having one. */
    p->smp_tilt = db_oct < -12.0 ? -12.0 : (db_oct > 12.0 ? 12.0 : db_oct);
}

void ae_corrector_set_sample_level (AeCorrector *p, double lin)
{
    p->smp_level = lin < 0.0 ? 0.0 : (lin > 16.0 ? 16.0 : lin);
}

void ae_corrector_set_sample_tone (AeCorrector *p, double db)
{
    p->smp_tone_db = db < -12.0 ? -12.0 : (db > 12.0 ? 12.0 : db);
}

/* The MEL preset table: each "instrument" is a parameter set, never
   stored audio -- ratios and gains of the resynthesized copies, a
   spectral shape per copy (one-pole LP), artifact depths (shared tape
   wow, flutter, saturation drive, master bandwidth) and how hard a new
   onset re-swells the envelope. The unison copy goes THROUGH the
   shifter bank at ratio 1: resynthesis smears the pick transient, which
   is most of why the layer stops sounding like the guitar. Ratios are
   harmonic (2:1, 3:1), so any tuning survives them; the tiny detunes
   (strings, choir) are ensemble, not temperament. */
static const AeMelPresetSpec k_mel_presets[] = {
    /* gain -1 in the octave slot = "use melOctDb" (the footage knob) */
    { "footage", 1.0, { { 0.0,  0.0,    0, false },
                        { 12.0, -1.0,   0, false },
                        { 0.0,  0.0,    0, false } }, 0.0, 0.0, 0.0,    0, 0.0 },
    { "organ",   0.0, { { 0.0,   1.0, 7000, false },
                        { 12.0,  0.5, 7000, false },
                        { 19.02, 0.35, 6000, false } }, 1.2, 0.4, 0.20, 7500, 0.35 },
    { "flute",   0.0, { { 0.0,   1.0, 1600, false },
                        { 12.0,  0.18, 2500, false },
                        { 0.0,   0.0,    0, false } }, 3.5, 0.9, 0.15, 5000, 0.50 },
    { "strings", 0.0, { { 0.0,   0.9, 5200, false },
                        { 12.0,  0.45, 5200, false },
                        { 0.18,  0.55, 4500, false } }, 2.5, 0.7, 0.20, 6500, 0.55 },
    { "brass",   0.0, { { 0.0,   1.0, 3800, true  },
                        { -12.0, 0.35, 2500, false },
                        { 12.0,  0.25, 3800, false } }, 1.5, 1.0, 0.35, 6000, 0.60 },
    { "choir",   0.0, { { 0.0,   0.9, 3400, false },
                        { 12.0,  0.4, 3000, false },
                        { -0.22, 0.6, 3400, false } }, 4.0, 0.5, 0.15, 5000, 0.45 },
    /* "auto": the spec is MEASURED from the loaded bank (mel_analyze_bank)
       and lives in p->mel_auto; this row is only the face shown before any
       bank has been analyzed -- the plain footage stack, so picking auto
       early is never silence. */
    { "auto",    1.0, { { 0.0,  0.0,    0, false },
                        { 12.0, -1.0,   0, false },
                        { 0.0,  0.0,    0, false } }, 0.0, 0.0, 0.0,    0, 0.0 },
};

/* The spec the render should follow: the measured bank timbre when the
   preset is "auto" and an analysis exists, the table otherwise. */
static const AeMelPresetSpec *mel_spec (const AeCorrector *p)
{
    const int n = (int) (sizeof (k_mel_presets) / sizeof (k_mel_presets[0]));
    if (p->mel_preset == n - 1)
    {
        const int g = atomic_load_explicit (
            &((AeCorrector *) p)->mel_auto_gen, memory_order_acquire);
        if (g > 0)
            return &p->mel_auto[(g - 1) & 1];
    }
    return &k_mel_presets[p->mel_preset >= 0 && p->mel_preset < n
                              ? p->mel_preset : 0];
}

/* MEL "auto" (field ask: "spectral analysis on the selected samples ...
   applied to the MEL algorithm to replicate the timbre"): measure the
   bank's own steady-state harmonic weights and derive the remodeller's
   parameters from them -- the harmonic-transfer-function move, with the
   table read off the actual recordings instead of tuned by hand.

   Control thread only (called from ae_corrector_load_samples). Publishes
   into the idle mel_auto buffer, then flips mel_auto_gen (release). */
static void mel_analyze_bank (AeCorrector *p, int bank_idx)
{
    const AeSampleBank *b = &p->smp_bank[bank_idx];
    if (b->n_recs <= 0)
        return;
    /* The record nearest middle C, main takes preferred: mid-register is
       where the timbre statement lives (extremes carry zone quirks). */
    const AeSampleRec *r = NULL;
    int best = 1 << 30;
    for (int i = 0; i < b->n_recs; ++i)
    {
        const AeSampleRec *c = &b->recs[i];
        const int d = abs (c->midi - 60) * 2 + (c->soft ? 1 : 0);
        if (c->pcm != NULL && c->len > 0 && d < best)
        {
            best = d;
            r = c;
        }
    }
    if (r == NULL)
        return;
    const double f0 = 440.0 * pow (2.0, (r->midi - 69) / 12.0);
    /* Steady state: inside the loop when one exists, else past the attack
       (a quarter in, capped at 200 ms). At least ~50 ms of material or
       the estimate is noise. */
    int start = r->loop_end > r->loop_start ? r->loop_start
              : (int) (0.2 * p->fs) < r->len / 4 ? (int) (0.2 * p->fs)
                                                 : r->len / 4;
    int n = r->len - start;
    if (n > (int) p->fs)
        n = (int) p->fs;
    if (n < (int) (0.05 * p->fs))
        return;
    /* Harmonic magnitudes by Goertzel at k * f0. */
    double m[17] = { 0 };
    double mmax = 0.0;
    for (int k = 1; k <= 16; ++k)
    {
        const double f = f0 * k;
        if (f > 0.45 * p->fs)
            break;
        const double w = 2.0 * M_PI * f / p->fs, c2 = 2.0 * cos (w);
        double s0, s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            s0 = (double) r->pcm[start + i] + c2 * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        m[k] = sqrt (s1 * s1 + s2 * s2 - c2 * s1 * s2) / n;
        if (m[k] > mmax)
            mmax = m[k];
    }
    if (mmax <= 0.0)
        return;
    for (int k = 1; k <= 16; ++k)
        m[k] /= mmax;

    /* Map onto the remodeller. The unison layer's LP: the guitar source
       arrives roughly 1/k, so the wanted response at k*f0 is ~k*m[k] --
       take the highest harmonic still holding half the peak of that
       curve as the corner. Octave layer from the even/odd balance,
       twelfth layer from the 3rd partial, bandwidth from the highest
       audible harmonic. Tape character stays fixed -- the wow is the
       MEDIUM, not the instrument. */
    double kw_max = 0.0;
    for (int k = 1; k <= 16; ++k)
        if (k * m[k] > kw_max)
            kw_max = k * m[k];
    int k_fc = 1;
    for (int k = 1; k <= 16; ++k)
        if (k * m[k] >= 0.5 * kw_max)
            k_fc = k;
    double fc = (k_fc + 0.5) * f0;
    if (fc < 800.0)  fc = 800.0;
    if (fc > 8000.0) fc = 8000.0;
    const double even = m[2] + m[4] + m[6];
    const double odd  = m[1] + m[3] + m[5];
    double g12 = even + odd > 1e-9 ? 0.9 * even / (even + odd) : 0.0;
    if (g12 > 0.8) g12 = 0.8;
    double g19 = m[1] > 1e-6 ? 0.5 * m[3] / m[1] : 0.0;
    if (g19 > 0.6) g19 = 0.6;
    int k_hi = 1;
    for (int k = 1; k <= 16; ++k)
        if (m[k] > 0.02)
            k_hi = k;
    double mlp = 1.2 * k_hi * f0;
    if (mlp < 2500.0) mlp = 2500.0;
    if (mlp > 9000.0) mlp = 9000.0;

    const int cur = atomic_load_explicit (&p->mel_auto_gen,
                                          memory_order_relaxed);
    const int nxt = cur > 0 ? ((cur - 1) & 1) ^ 1 : 0;
    AeMelPresetSpec *s = &p->mel_auto[nxt];
    memset (s, 0, sizeof (*s));
    s->name = "auto";
    s->g_dry = 0.0;
    s->layer[0] = (AeMelLayerSpec) { 0.0,   1.0, fc,        false };
    s->layer[1] = (AeMelLayerSpec) { 12.0,  g12, fc,        false };
    s->layer[2] = (AeMelLayerSpec) { 19.02, g19, fc * 0.9,  false };
    s->wow_c = 2.5;
    s->flut_c = 0.8;
    s->drive = 0.2;
    s->master_lp = mlp;
    s->retrig = 0.5;
    atomic_store_explicit (&p->mel_auto_gen, nxt + 1, memory_order_release);
}

int ae_corrector_mel_presets (void)
{
    return (int) (sizeof (k_mel_presets) / sizeof (k_mel_presets[0]));
}

const char *ae_corrector_mel_preset_name (int i)
{
    return i >= 0 && i < ae_corrector_mel_presets () ? k_mel_presets[i].name
                                                     : NULL;
}

void ae_corrector_set_mel (AeCorrector *p, double mix, double oct_lin,
                           double atk_ms, double rel_ms, int preset)
{
    p->mel_mix    = mix < 0.0 ? 0.0 : (mix > 1.0 ? 1.0 : mix);
    p->mel_oct_g  = oct_lin < 0.0 ? 0.0 : (oct_lin > 4.0 ? 4.0 : oct_lin);
    p->mel_atk_ms = atk_ms < 0.0 ? 0.0 : (atk_ms > 5000.0 ? 5000.0 : atk_ms);
    p->mel_rel_ms = rel_ms < 5.0 ? 5.0 : (rel_ms > 10000.0 ? 10000.0 : rel_ms);
    p->mel_preset = preset < 0 ? 0
                  : (preset >= ae_corrector_mel_presets () ? 0 : preset);
}

void ae_corrector_set_lead_gain (AeCorrector *p, double lin)
{
    p->lead_gain = lin < 0.0 ? 0.0 : (lin > 4.0 ? 4.0 : lin);
}

void ae_corrector_set_steel (AeCorrector *p, bool on, int root_deg,
                             double level_lin)
{
    p->steel_on    = on;
    p->steel_root  = root_deg < 0 ? 0 : root_deg;
    p->steel_level = level_lin < 0.0 ? 0.0 : (level_lin > 4.0 ? 4.0 : level_lin);
}

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

/* A bass-TYPE set by name: "bass" anywhere in the folder name -- which is
   what makes a user-added "synthbass" or "mellotronbass" just work -- with
   the one English trap excluded (a bassoon is not a bass). */
static bool instrument_is_bass (const char *name)
{
    for (const char *s = name; *s != '\0'; ++s)
    {
        if (tolower ((unsigned char) s[0]) != 'b')
            continue;
        if (tolower ((unsigned char) s[1]) == 'a'
            && tolower ((unsigned char) s[2]) == 's'
            && tolower ((unsigned char) s[3]) == 's')
        {
            if (tolower ((unsigned char) s[4]) == 'o'
                && tolower ((unsigned char) s[5]) == 'o'
                && tolower ((unsigned char) s[6]) == 'n')
                continue; /* bassoon: keep looking past it */
            return true;
        }
    }
    return false;
}

/* Resolve smp_register against the LIVE bank's name. Called wherever either
   side changes (a bank load, the key's write); the ratio is what the audio
   thread reads, so the resolution never runs per strike. */
static void sample_register_resolve (AeCorrector *p)
{
    int st = p->smp_register;
    if (st == AE_SMP_OCTAVE_AUTO)
    {
        const int live = atomic_load_explicit (&p->smp_live,
                                               memory_order_acquire);
        st = (live >= 0 && instrument_is_bass (p->smp_bank[live].instrument))
                 ? -12 : 0;
    }
    p->smp_reg_ratio = pow (2.0, (double) st / 12.0);
}

void ae_corrector_set_sample_register (AeCorrector *p, int semitones)
{
    p->smp_register = semitones;
    sample_register_resolve (p);
}

static void mel_analyze_bank (AeCorrector *p, int bank_idx);

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
    sample_register_resolve (p); /* the register follows the NAME just loaded */
    mel_analyze_bank (p, idle);  /* melPreset "auto" learns this bank */
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

/* Strike-to-strike smoothing: pull a measured level to within
   AE_VEL_CLAMP_DB of the running average of recent verdicts. The first
   note (no history) is taken as played. */
static double vel_clamp_level (const AeCorrector *p, double level)
{
    if (level <= 1e-9 || p->vel_ema_db < -900.0)
        return level;
    double db = 20.0 * log10 (level);
    if (db > p->vel_ema_db + AE_VEL_CLAMP_DB)
        db = p->vel_ema_db + AE_VEL_CLAMP_DB;
    else if (db < p->vel_ema_db - AE_VEL_CLAMP_DB)
        db = p->vel_ema_db - AE_VEL_CLAMP_DB;
    return pow (10.0, db / 20.0);
}

/* The smoothed body-level estimate available RIGHT NOW -- what a strike
   uses before the verdict window has closed (the fast follower's reading,
   tamed by the same clamp the verdict wears, so the estimate and the
   verdict cannot be a leap apart). Peak-equivalent units throughout. */
static double body_level_now (const AeCorrector *p)
{
    return vel_clamp_level (p, p->atk_fast * AE_SQRT2);
}

/* The strike gain before the per-instrument trim, from a measured body
   level: the relative velocity map (0.2..1 against the rolling reference)
   crossfaded in the dB domain toward SOURCE PARITY -- the gain that puts
   the sample's body (bank-normalised to AE_SMP_BODY_RMS) at the level the
   note was actually played. sampleMatch picks the blend; a host-pinned
   sampleVelocity bypasses parity entirely, because a pinned strike level
   is a pinned strike level. Parity is clamped to -34..+12 dB so a noise
   floor can never be renormalised into fortissimo. */
/* The strike measurement's pitch weighting (strikeTilt): raw RMS is not
   loudness, and a guitar's low strings carry far more of it than the ear
   credits -- matching raw RMS made low notes strike loud and high notes
   vanish. Discount below the pivot, credit above, on the MEASUREMENT
   only; hz 0 (nothing detected yet) passes the level through. */
static double tilt_level (const AeCorrector *p, double level, double hz)
{
    if (p->smp_tilt == 0.0 || hz <= 0.0 || level <= 1e-9)
        return level;
    return level
         * pow (10.0, p->smp_tilt * log2 (hz / AE_VEL_TILT_PIVOT_HZ) / 20.0);
}

static double strike_gain (const AeCorrector *p, double level, double hz)
{
    level = tilt_level (p, level, hz);
    const double vel = p->smp_vel_fixed >= 0.0 ? p->smp_vel_fixed
                                               : vel_from_peak (p, level);
    const double m = p->smp_match;
    if (m <= 0.0 || p->smp_vel_fixed >= 0.0)
        return vel;
    double parity = (level > 1e-9 ? level / AE_SQRT2 : 0.0) / AE_SMP_BODY_RMS;
    if (parity < 0.02) parity = 0.02;
    if (parity > 8.0)  parity = 8.0; /* was 4.0: the tilt's high-note
        credit ran into this ceiling and flattened -- max tilt asked for
        gain the clamp then refused, which read as "tilt does nothing up
        there". +18 dB headroom; the strike-to-strike clamp still tames
        any one wild measurement. */
    if (m >= 1.0)
        return parity;
    return pow (10.0, ((1.0 - m) * log10 (vel > 1e-9 ? vel : 1e-9)
                       + m * log10 (parity)));
}

/* Strike one voice: pick the recording, set the read cursor and the
   fractional rate. The OLD slot is left ringing on a 6 ms fade rather than
   cut, which is the sampler equivalent of Xentar's node-swap retrigger --
   never interrupt a live voice, crossfade past it. */
/* `wrel` is the strike's relative weight inside its event -- a chord
   member's share of the strum (already folded into `vel` by the caller),
   1.0 for a lone note. It travels with the slot so the velocity verdict's
   refinement can re-scale the EVENT's level without flattening the chord. */
static void sample_strike (AeCorrector *p, int v, double hz, double vel,
                           double wrel)
{
    const int live = atomic_load_explicit (&p->smp_live, memory_order_acquire);
    if (live < 0 || hz <= 0.0)
        return;
    const AeSampleBank *bank = &p->smp_bank[live];
    if (bank->n_recs == 0)
        return;

    /* REGISTER: a bass-type set plays the bass register -- the strike
       lands an octave below the pitch asked for (sampleRegister; the
       repitch wears the same ratio, so a bend rides in register too). */
    hz *= p->smp_reg_ratio > 0.0 ? p->smp_reg_ratio : 1.0;

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

    const double trim = p->smp_trim > 0.0 ? p->smp_trim : 1.0;
    const double rec_hz = 440.0 * pow (2.0, (rec->midi - 69) / 12.0);
    p->smp[v][nxt].rec  = rec;
    p->smp[v][nxt].pos  = 0.0;
    /* Fractional and UNQUANTISED -- that is what lands a 22-EDO degree
       exactly off a 12-per-octave map. */
    p->smp[v][nxt].rate = hz / rec_hz;
    p->smp[v][nxt].gain   = vel * trim;
    p->smp[v][nxt].gain_t = vel * trim;
    /* The bank's measured level travels with the strike, so a slot still
       ringing from the previous instrument keeps ITS normalisation. */
    p->smp[v][nxt].norm   = bank->norm;
    p->smp[v][nxt].fade = 1.0;
    p->smp[v][nxt].wrel  = wrel > 0.0 ? wrel : 1.0;
    p->smp[v][nxt].epoch = p->vel_epoch;
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
    /* A sustained recording WRAPS inside its steady state instead of ending
       when the file does: the seam crossfade is already baked into the PCM,
       so this is a plain index move with nothing to blend per sample, and
       the note holds the tone it was actually playing rather than stopping
       mid-bow. Recordings without a detected loop keep the old behaviour --
       run off the end and free the slot -- because a plucked or struck
       sound's decay IS the sound and looping it would be wrong. */
    if (r->loop_end > r->loop_start && r->loop_end <= r->len
        && p->smp[v][slot].pos >= (double) r->loop_end)
    {
        const double len = (double) (r->loop_end - r->loop_start);
        p->smp[v][slot].pos = (double) r->loop_start
            + fmod (p->smp[v][slot].pos - (double) r->loop_start, len);
    }
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
            /* Freed at -80 dB, not -60: a slot cut at -60 dB is a step a
               dense sustained texture (choir) makes audible, especially
               after the bank's makeup gain -- "fades out, then cuts off
               at a quiet volume". At -80 the exponential has genuinely
               finished. */
            if (p->smp[v][k].renv < 1e-4)
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
    hz *= p->smp_reg_ratio > 0.0 ? p->smp_reg_ratio : 1.0; /* stay in register */
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

    /* FOLLOW level: the fast follower as a peak estimate, absolute, and
       gated to a clean zero so "note stopped" transmits as silence rather
       than as the noise floor. */
    {
        const double lvl = p->atk_fast > GATE (p)
                               ? p->atk_fast * AE_SQRT2 : 0.0;
        atomic_store_explicit (&p->env_out,
                               (float) (lvl > 1.0 ? 1.0 : lvl),
                               memory_order_relaxed);
    }

    /* One pulse per onset EDGE (Schmitt: re-arms only when the fast/slow
       ratio collapses, so a refractory expiring mid-note cannot double). */
    if (! p->atk_armed
        && (p->atk_fast < 1.2 * p->atk_slow || p->atk_fast < GATE (p)))
        p->atk_armed = true;

    p->onset_pulse = false;
    if (p->atk_armed && p->atk_refract <= 0
        && p->atk_fast > 2.0 * GATE (p)
        && p->atk_fast > 2.5 * slow_prev)
    {
        p->onset_pulse = true;
        ++p->vel_epoch; /* strikes from here belong to THIS onset; the
                           verdict below refines only these */
        /* A new energy edge is a new event: the detector's octave-
           continuity hysteresis (a raised bar for CHANGING octave
           mid-note) is a claim about the note that just ended, and
           carrying it across the boundary makes the first frames of a
           leap fight the previous note's octave. Clear it; the new note
           earns its own continuity. */
        p->detector.last_best_tau = 0;
        p->atk_armed   = false;
        p->atk_refract = (int) (0.060 * p->fs);
        p->vel_win     = (int) (AE_VEL_BODY_S * p->fs);
        p->vel_peak    = 0.0;
        p->vel_sq      = 0.0;
        p->vel_n       = 0;
        /* ...and arm the ATTACK HOLD on the quantizer (see run_detection):
           a plucked string starts sharp, and the snap must not believe
           the first hops of the transient over the note that was already
           sounding. */
        if (p->attack_hold <= 0)
            ++p->att_seq;
        p->attack_hold = (int) (0.080 * p->fs);
    }
    if (p->attack_hold > 0)
        p->attack_hold -= num_samples;
    if (p->att_hold_recent > 0)
        p->att_hold_recent -= num_samples;

    if (p->vel_win > 0)
    {
        if (peak > p->vel_peak) p->vel_peak = peak;
        p->vel_sq += sum;
        p->vel_n  += num_samples;
        p->vel_win -= num_samples;
        if (p->vel_win <= 0)
        {
            /* The verdict: the BODY's RMS as a peak-equivalent (x sqrt2),
               not the pick's instantaneous peak -- and clamped against the
               running average of recent verdicts before anything reads it,
               so one odd measurement is a nudge, never a leap (field:
               "very touchy... too many very loud or very quiet notes").
               The clamped value then FEEDS the average, which lets a real
               dynamic change walk the window where it wants in a few
               notes. */
            const double raw = (p->vel_n > 0
                                    ? sqrt (p->vel_sq / (double) p->vel_n)
                                    : 0.0) * AE_SQRT2;
            const double lvl = vel_clamp_level (p, raw);
            /* The clamped verdict becomes the anchor for the next one, so
               the rule is simply "a verdict moves at most AE_VEL_CLAMP_DB
               per note": one odd measurement is tamed, a deliberate
               dynamic change walks the whole way in two or three notes. */
            if (lvl > 1e-9)
                p->vel_ema_db = 20.0 * log10 (lvl);
            const double vel = vel_from_peak (p, lvl);
            /* A measured onset is the only thing that RAISES an OBSERVED
               reference, and it does so after being mapped -- so the
               hardest note in the last ~20 s reads 1.0 and sets the bar for
               the rest. A supplied one is never raised; the max() inside
               the map still keeps a louder-than-reference note at unity
               rather than above it. */
            if (p->vel_ref_fixed < 0.0 && lvl > p->vel_ref)
                p->vel_ref = lvl;
            atomic_store_explicit (&p->smp_vel_out, (float) vel, memory_order_relaxed);
            /* Refine the strike this window was measuring -- the LEVEL of
               the note now sounding, not the next one's. Only the level:
               the layer (soft vs main) is a different recording and was
               committed at the strike, so a note whose estimate landed the
               wrong side of the soft threshold keeps that timbre. It is
               the one thing measuring cannot fix without delaying the
               strike, which is the latency the feature exists to hide.
               The refinement rides the same map-vs-parity blend the strike
               used, and keeps the per-instrument trim it used to drop. */
            if (p->smp_vel_fixed < 0.0)
            {
                const double g = strike_gain (p, lvl,
                                              (double) ae_corrector_detected_hz (p))
                               * (p->smp_trim > 0.0 ? p->smp_trim : 1.0);
                /* Only slots struck AT THIS ONSET (epoch match): a note
                   still ringing from an earlier strike was not what this
                   window measured, and re-scaling it is an audible level
                   jump mid-ring (poly rows hold independent notes, so
                   this was routine there). The slot's relative weight
                   rides along -- refining the strum's level must not
                   flatten the chord. */
                for (int v = 0; v <= AE_HARM_VOICES + 1; ++v)
                {
                    const int cur = p->smp_cur[v];
                    if (p->smp[v][cur].rec != NULL
                        && p->smp[v][cur].epoch == p->vel_epoch)
                        p->smp[v][cur].gain_t = g * p->smp[v][cur].wrel;
                }
            }
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
    const bool now_voiced = res.voiced && res.frequency_hz > 0.0 && rms > GATE (p);

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
            /* Inside the attack window the motion IS the transient
               settling (vibrato has not started yet): follow at ~40 ms
               so a sharp attack's centre reaches the true note in a few
               hops instead of pinning a wrong degree for ~150 ms. */
            p->centre_cents += (detected_cents - p->centre_cents)
                             * (1.0 - exp (-elapsed /
                                    (p->attack_hold > 0 ? 0.040 : 0.180)));
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

        /* Re-voicing while a note's degree is still remembered IS a
           re-attack, whatever the energy Schmitt thought -- and it must
           arm BEFORE the quantizer runs this hop, or the transient snaps
           first and poisons the remembered degree with its own commit. */
        if ((! p->voiced || ! p->primed) && p->att_hold_recent > 0)
        {
            if (p->attack_hold <= 0)
                ++p->att_seq;
            p->attack_hold = (int) (0.080 * p->fs);
        }

        /* Re-plucks the energy onset MISSES (string still ringing, so the
           fast/slow contrast is small) still announce themselves in
           pitch: the transient reads sharp. The analysis window smears
           its rise across 2-3 hops (~13 cents each -- under any safe
           per-hop threshold), so the comparison runs THREE hops back
           (~15 ms), where the re-pluck's rise accumulates to +25..40
           cents while a bend covers a few. Deep fast vibrato can reach
           the threshold too, and arming on it is harmless: the hold only
           refuses ADJACENT-degree moves while still within 3/4 step of
           the current target -- exactly the wobble that should hold. */
        if (p->att_prev_valid
            && fabs (detected_cents - p->att_hist[2]) > 18.0)
        {
            if (p->attack_hold <= 0)
                ++p->att_seq;
            p->attack_hold = (int) (0.080 * p->fs);
        }
        p->att_hist[2] = p->att_hist[1];
        p->att_hist[1] = p->att_hist[0];
        p->att_hist[0] = detected_cents;
        if (! p->att_prev_valid)
        {
            p->att_hist[1] = p->att_hist[2] = detected_cents;
            p->att_prev_valid = true;
        }

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

        /* Is the previous target still eligible to be held onto? (Shared
           by stickiness and the attack hold below.) */
        bool last_ok = false;
        if (p->target_valid && cand != p->target_j)
        {
            if (midi_active)
            {
                for (int i = 0; i < held_n; ++i) /* must still be held */
                    if (held_j[i] == p->target_j)
                        last_ok = true;
            }
            else
            {
                const int last_deg = (int) (((p->target_j % p->edo) + p->edo) % p->edo);
                last_ok = p->enabled_deg[last_deg];
            }
        }

        /* Stickiness (hysteresis): stay on the previous target until the
           detected pitch has travelled past the midpoint toward the new
           candidate by an extra `stickiness` fraction of the half-gap.
           Kills degree flicker when the step is smaller than vibrato. */
        if (p->target_valid && cand != p->target_j && p->stickiness > 0.0
            && last_ok
            && fabs (steps - (double) p->target_j)
                 < (0.5 + 0.5 * p->stickiness) * fabs ((double) (cand - p->target_j)))
            cand = p->target_j;

        /* ATTACK HOLD: a plucked string STARTS SHARP -- tension modulation
           reads +20..+40 cents on the first hops of every pluck, settling
           over ~50 ms -- so inside a short window after each onset, a
           pitch still within 3/4 of a step of the note that was ALREADY
           SOUNDING is that note RE-PLUCKED, not a new degree: hold. A
           genuinely different note reads past the threshold even
           mid-transient (one step is a full 1.0 away) and commits
           immediately, so real moves pay no latency.

           The reference degree is att_hold_j, remembered for ~150 ms,
           NOT target_j: a pick transient costs 1-2 unvoiced hops, and
           the fresh-onset reset wipes target_valid at exactly the moment
           the hold is needed most (measured: tv 0 on the re-pluck hop,
           fresh snap into the sharp transient, one step up). Without all
           of this, repeated plucks of one note flipped the target a step
           UP at every attack whenever the string sat a few cents sharp
           of the degree -- in 22-EDO (54.5-cent steps, 27.3 to the
           boundary) that is much of a real guitar's fretting. */
        if (p->attack_hold > 0 && p->att_hold_recent > 0)
        {
            /* the memory only -- never the degree the attack itself just
               committed, or a wrong first snap would defend itself */
            const long long hj = p->att_hold_j;
            if (cand != hj && fabs (steps - (double) hj) < 0.75)
            {
                bool ok = false;
                if (midi_active)
                {
                    for (int i = 0; i < held_n; ++i)
                        if (held_j[i] == hj)
                            ok = true;
                }
                else
                    ok = p->enabled_deg[(int) (((hj % p->edo) + p->edo)
                                               % p->edo)];
                if (ok)
                    cand = hj;
            }
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
        atomic_store_explicit (&p->lead_deg_out,
                               (int) (cand + p->lead_shift), memory_order_relaxed);

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
        /* The degree that survives a pick's brief unvoiced dip.
           Refreshed only from STABLE hops: a degree committed inside an
           attack window is the transient's guess, not a note that was
           sounding, and must not become the thing the next hold
           protects. */
        if (p->attack_hold <= 0)
        {
            p->att_hold_j      = p->target_j;
            p->att_hold_recent = (int) (0.150 * p->fs);
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
        /* The release slope-freeze verdict, decided HERE so the exported
           pitch can honour it too (its full story sits at the harmony
           freeze below). */
        {
            const int rb  = (p->rel_pos + AE_REL_RING - 8) % AE_REL_RING;
            const int rbl = (p->rel_pos + AE_REL_RING - 15) % AE_REL_RING;
            /* Three faces of a release (field, precise: "when I stop
               pressing the string down but keep my finger on it, it
               bends down; a right-hand mute doesn't"):
               - a DAMP: level collapses fast (~110 dB/s over 40 ms);
               - a slow damp: the same collapse read over 75 ms;
               - a FINGER-LIFT: the string genuinely detunes FLAT as the
                 fret grip eases while the level falls only gently --
                 flat of centre while clearly decaying is a lift, never
                 a bend (an intentional bend sustains its level, and an
                 upward bend is never flat). */
            const bool fast_c = p->rel_rms[rb] > 0.0f
                             && rms < 0.6 * (double) p->rel_rms[rb];
            const bool slow_c = p->rel_rms[rbl] > 0.0f
                             && rms < 0.55 * (double) p->rel_rms[rbl];
            const bool lift_c = p->rel_rms[rbl] > 0.0f
                             && rms < 0.85 * (double) p->rel_rms[rbl]
                             && p->expr_cents < -12.0;
            p->rel_collapse = fast_c || slow_c || lift_c;
        }
        /* The corrected pitch WITH the playing on top -- what the shifter
           aims at, exported so a sample lead can ride a physical bend
           instead of stair-stepping the snapped grid. HELD through a
           collapse: a damped string's pitch dives while still reading
           voiced, and a ringing sample that followed it bent audibly
           down on every release (field, with the pitch trace to prove
           it). The freeze the ghosts always had, extended to the export. */
        if (! p->rel_collapse)
            atomic_store_explicit (&p->corr_hz_out,
                                   (float) (ref * pow (2.0, (out_expr + lead_shift_c)
                                                            / 1200.0)),
                                   memory_order_relaxed);
        /* Bend-vs-jump: a hammer-on moves a step in one hop, a bend
           creeps. Peak-hold (~25 ms at the 5 ms hop) so the block-rate
           reader downstream still sees the jump hop. */
        {
            const double dsl = p->prev_pair_valid
                ? fabs (detected_cents - p->prev_det_cents) : 0.0;
            p->det_slope_hold *= 0.8;
            if (dsl > p->det_slope_hold)
                p->det_slope_hold = dsl;
        }

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
        if (p->rel_collapse)
            goto harmony_done;

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
        p->att_prev_valid  = false; /* silence breaks the jump comparison */
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
    if (! now_voiced)
        atomic_store_explicit (&p->lead_deg_out, AE_HARM_DEG_OFF,
                               memory_order_relaxed);

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

static double sample_tone_tick (AeCorrector *p, int s, double x);
static void   sample_layer_gains (double mix, double *g_dry, double *g_smp);

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
       against a pathological match on near-silent gated input) -- then the
       consolidated SYNTH/SAMPLE section's fader and tone, exactly as the
       sample voices wear them (field: synth sounds lost their level control
       when leadSoundGainDb retired -- "not working or very quiet"). */
    const double level = (p->harm_hold && p->hold_latched) ? p->hold_level
                                                           : p->in_level;
    const double match = dclamp (level / synth_patch_rms (pat), 0.0, 4.0)
                       * p->smp_level;
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
        /* the section's mix taper and tone, exactly as sample ghosts wear
           them -- one SYNTH/SAMPLE section, one law */
        double g_dry_u, g_smp;
        sample_layer_gains (p->smp_mix, &g_dry_u, &g_smp);
        (void) g_dry_u;
        for (int i = 0; i < num_samples; ++i)
        {
            env += (want - env) * a;
            const double s = sample_tone_tick (p, v, buf[i])
                           * env * match * g_smp;
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
/* The sample mix's two sides on the shared taper (0.5 = both at full,
   never a crossfade). The first side is the DRY guitar now -- the
   latency-free input block, mixed at delivery -- not the shifted path it
   used to be: the player's hands must never wait on the detector. */
static void sample_layer_gains (double mix, double *g_dry, double *g_smp)
{
    const double a = mix <= 0.5 ? 1.0 : 2.0 * (1.0 - mix);
    const double b = mix >= 0.5 ? 1.0 : 2.0 * mix;
    *g_dry = sin (a * (AE_PI / 2.0));
    *g_smp = sin (b * (AE_PI / 2.0));
}

/* sampleToneDb: the harmony bus's tilt (render_tilt), cloned onto the
   sample voices only -- one state per voice row, `s` = AE_HARM_VOICES for
   the lead/chord row. Neutral costs one compare. */
static double sample_tone_tick (AeCorrector *p, int s, double x)
{
    if (p->smp_tone_db == 0.0)
        return x;
    if (p->smp_tone_db != p->smp_tone_db_cur)
    {
        const double half = p->smp_tone_db * 0.5;
        p->smp_tone_g_hi = pow (10.0, half / 20.0);
        p->smp_tone_g_lo = pow (10.0, -half / 20.0);
        p->smp_tone_db_cur = p->smp_tone_db;
    }
    double lp = p->smp_tone_lp[s];
    lp += p->tilt_a * (x - lp);
    p->smp_tone_lp[s] = lp;
    return p->smp_tone_g_lo * lp + p->smp_tone_g_hi * (x - lp);
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
    double g_dry_unused, g_smp;
    sample_layer_gains (p->smp_mix, &g_dry_unused, &g_smp);
    (void) g_dry_unused; /* the dry side is mixed once, at lead delivery */
    const double g_v = g_smp * p->smp_level;

    /* The strike level. Measuring takes the body window and the strike
       cannot wait for it, so a struck voice takes the fast follower's
       reading NOW -- tamed by the same strike-to-strike clamp the verdict
       wears -- and the window's verdict corrects it when it closes (see
       detect_onset). Reading smp_vel_out here instead would strike every
       note at the PREVIOUS note's level, which is exactly wrong the moment
       the dynamics change. */
    const double vel = strike_gain (p, body_level_now (p),
                                    (double) ae_corrector_detected_hz (p));
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
                sample_strike (p, v, hz, vel, 1.0);
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
            const double x =
                sample_tone_tick (p, v, sample_mix_slots (p, v, fade_step, rel_a));
            const double sv = x * env * g_v;
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
    /* The FOLLOW gain rides the master's own ~5 ms smoothing; the ghosts
       are anchored to the followed lead, so they cut with it. */
    const double target = p->harm_master * p->follow_gain_cur;
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

/* STEEL mode: the pedal-steel dyad on the guitar itself. While the lead
   is voiced, a ROOT DRONE sounds under the playing -- the root class at
   the nearest degree BELOW the audible lead (an equave down when the
   note IS the root, so the pair is never a unison). The player bends
   their own string over it; the drone holds until the playing ends,
   then decays under the lead release. A sample lead drones the loaded
   bank on its own row; synth and shifted leads drone the synth patch on
   a dedicated voice (the chart's droneOn rank stays untouched). Renders
   the block into steel_buf; the caller adds it to the lead bus, so the
   lead IR sculpts it too. */
static void steel_process (AeCorrector *p, bool voiced, long lead_deg,
                           bool sample_src, int num_samples)
{
    const int S = AE_HARM_VOICES + 1;
    if (p->steel_buf == NULL)
        return;
    memset (p->steel_buf, 0, (size_t) num_samples * sizeof (float));
    if (! p->steel_on && p->steel_deg == AE_HARM_DEG_OFF
        && p->steel_env < 1e-4 && sample_voice_idle (p, S))
        return; /* fully idle: nothing rings, nothing to do */

    const double period = p->period_cents > 0.0 ? p->period_cents : 1200.0;
    const double ref    = p->ref_hz > 0.0 ? p->ref_hz : AE_REFERENCE_C0_HZ;
    const int    edo    = p->edo > 0 ? p->edo : 12;

    long deg = AE_HARM_DEG_OFF;
    if (p->steel_on && voiced && lead_deg != AE_HARM_DEG_OFF)
    {
        const long root = ((long) p->steel_root % edo + edo) % edo;
        const long cls  = ((lead_deg % edo) + edo) % edo;
        long diff = ((cls - root) % edo + edo) % edo;
        if (diff == 0)
            diff = edo; /* on the root: drop an equave, never a unison */
        deg = lead_deg - diff;
    }

    const bool sample_live = sample_src
        && atomic_load_explicit (&p->smp_live, memory_order_acquire) >= 0;
    if (deg != p->steel_deg)
    {
        if (deg == AE_HARM_DEG_OFF)
        {
            /* the playing ended: the ringing drone closes under the
               release ceiling (the envelope below carries the synth) */
            for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
                if (p->smp[S][sl].rec != NULL)
                    p->smp[S][sl].releasing = true;
        }
        else
        {
            p->steel_hz = ae_degree_hz (deg, edo, ref, period);
            if (sample_live)
                sample_strike (p, S, p->steel_hz,
                               strike_gain (p, body_level_now (p), p->steel_hz)
                                   * p->steel_level,
                               1.0);
        }
        p->steel_deg = deg;
    }

    const double la  = p->lead_attack_ms  > 5.0 ? p->lead_attack_ms  : 5.0;
    const double lr  = p->lead_release_ms > 5.0 ? p->lead_release_ms : 5.0;
    const double atk = 1.0 - exp (-1.0 / (la * 0.001 * p->fs));
    const double rel = 1.0 - exp (-1.0 / (lr * 0.001 * p->fs));
    const double tgt = deg != AE_HARM_DEG_OFF ? 1.0 : 0.0;
    double g_dry_u, g_smp;
    sample_layer_gains (p->smp_mix, &g_dry_u, &g_smp);
    (void) g_dry_u;

    double env = p->steel_env;
    if (sample_live || ! sample_voice_idle (p, S))
    {
        /* sample drone: the row rings under the lead's release ceiling,
           through the section's tone and gains like every sample voice */
        const double fade_step = 1.0 / (0.006 * p->fs);
        for (int i = 0; i < num_samples; ++i)
        {
            env += (tgt - env) * (tgt > env ? atk : rel);
            double x = sample_mix_slots (p, S, fade_step, rel);
            x = sample_tone_tick (p, S, x);
            p->steel_buf[i] = (float) (x * env * g_smp * p->smp_level);
        }
    }
    else if (p->steel_hz > 0.0 && (tgt > 0.0 || env > 1e-4))
    {
        /* synth drone: the patch held at the drone pitch, volume-matched
           to the playing like every synth voice, under the same section */
        const AeSynthPatch *pat = &k_synth_patches[p->synth_patch];
        synth_render_voice (p, pat, p->steel_hz, p->steel_phase,
                            p->steel_lfo, &p->steel_lp,
                            p->steel_buf, num_samples);
        const double match = dclamp (p->in_level / synth_patch_rms (pat),
                                     0.0, 4.0)
                           * p->smp_level * g_smp * p->steel_level;
        for (int i = 0; i < num_samples; ++i)
        {
            env += (tgt - env) * (tgt > env ? atk : rel);
            p->steel_buf[i] = (float) (sample_tone_tick (p, S,
                                           (double) p->steel_buf[i])
                                       * env * match);
        }
    }
    p->steel_env = env;
}

/* POLY mode: the whole (chordal) input through fixed-ratio shifters --
   the pedal "poly" contract. No detection, no snapping, no per-note
   anything: leadShiftSteps moves the chord, each harmony voice is its
   interval applied to the chord, and the doubled analysis block (set in
   prepare) is what buys chord fidelity, at poly mode's documented
   latency price. Detection-dependent machinery -- correction, masks,
   MIDI Harmony, HOLD, sustain, synth/sample sources, attack sound, the
   FOLLOW sender's note -- is inert here; the envelope (and so envelope
   FOLLOW and the record send) still works, because energy needs no
   pitch. */
static void process_poly (AeCorrector *p, float *mono, float *harm_l,
                          float *harm_r, float *wet_out, int num_samples)
{
    memcpy (p->in_block, mono, (size_t) num_samples * sizeof (float));
    for (int i = 0; i < num_samples; ++i)
    {
        p->in_buf[p->in_write & p->buf_mask] = mono[i];
        ++p->in_write;
    }
    detect_onset (p, num_samples); /* env_out, the gate, the vel reference */

    const bool gate = p->atk_fast > GATE (p);
    p->voiced = gate;
    atomic_store_explicit (&p->voiced_out, gate, memory_order_relaxed);
    atomic_store_explicit (&p->detected_hz_out, 0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->target_hz_out,   0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->corr_hz_out,     0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->lead_deg_out, AE_HARM_DEG_OFF,
                           memory_order_relaxed);

    const double period = p->period_cents > 0.0 ? p->period_cents : 1200.0;
    const double step_c = p->edo > 0 ? period / (double) p->edo : 100.0;
    const double lead_st = (double) p->lead_shift * step_c / 100.0;
    atomic_store_explicit (&p->shift_st_out, (float) lead_st,
                           memory_order_relaxed);

    /* Lead: the chord, shifted -- or, with leadSource:"sample" and a bank
       loaded, the chord PLAYED BY THE LIBRARY (the MEL9 move): the poly
       tracker's notes strike sample voices at their EDO-snapped degrees,
       tracker slot k living on sample row k so the strike / repitch /
       let-ring / release-ceiling machinery is reused unchanged.
       sampleMix keeps its meaning -- 1 replaces the playing, 0.5 layers
       the library under the shifted chord. */
    const bool chord_sampler =
        p->lead_source == AE_HARM_SRC_SAMPLE
        && atomic_load_explicit (&p->smp_live, memory_order_acquire) >= 0;
    /* The dry side follows the CONFIG intent, not the bank: a sample lead
       whose library failed to load must still pass the instrument through
       at the mix's dry gain, or a bad path silences the player. */
    double g_dry_lead = 0.0, g_smp_lead = 0.0;
    if (p->lead_source == AE_HARM_SRC_SAMPLE)
        sample_layer_gains (p->smp_mix, &g_dry_lead, &g_smp_lead);

    /* The tracker runs whenever poly mode does -- not only for the chord
       sampler. The detection is EXPORTED (poly_note_out / status
       polyDetected): a host can read the chord for its own purposes --
       chord display, a polyphonic tuner, driving its own instruments --
       whether or not a bank is striking here.

       Feed it the freshest full window, once per quarter window: multi-f0
       wants overlap, and the strike machinery wants note edges no later
       than the tracker can honestly know them. An ONSET doubles the rate
       for 150 ms (onset-first staging: the event is known in
       milliseconds, so spend the frames where the notes are changing)
       and arms the re-strum check below. */
    if (p->onset_pulse)
    {
        p->poly_burst = (int) (0.150 * p->fs);
        /* The MEL swell re-triggers PER ONSET (the Super Ego half of the
           9-series lineage): a partial dip, so each attack blooms without
           pumping the pad under a ringing chord. Sampler or not -- the
           layer is poly's instrument either way. */
        p->mel_env *= 1.0 - mel_spec (p)->retrig;
        if (chord_sampler)
        {
            /* Arm the re-strum window: freeze each row's raw salience as
               the pre-pluck baseline. The window spans the analysis
               window's fill time, so the jump has time to show. Only
               rows ALREADY SOUNDING at the onset take part -- a row that
               births during the window is a new note and the birth
               strike owns it (with a zero baseline it would otherwise
               restrike immediately: a double strike on every attack). */
            p->poly_restrike = (int) (0.200 * p->fs);
            for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
            {
                p->poly_base_raw[k] = p->polyf0.notes[k].raw;
                p->poly_refired[k]  = ! p->polyf0.notes[k].active;
            }
            /* ...and start the ANSWER DEADLINE: an onset the sampler has
               not answered ~70 ms from now (no birth, no attributed
               restrike) was a re-strum whose salience jump drowned in a
               still-loud ring -- the per-note 1.3x test's blind spot,
               which played as "randomly produces no output". The notes
               provably sounding get re-struck then; a retrigger of
               already-validated pitches cannot be a wrong note. 70 ms
               gives the burst-rate tracker ~6 frames to attribute the
               onset properly first. */
            p->poly_fb     = (int) (0.070 * p->fs);
            p->poly_fb_due = false;
        }
    }
    p->poly_fill += num_samples;
    const int hop = p->polyf0.win_size / (p->poly_burst > 0 ? 8 : 4);
    if (p->poly_burst > 0)
        p->poly_burst -= num_samples;
    if (p->poly_restrike > 0)
        p->poly_restrike -= num_samples;
    if (p->poly_fb > 0)
    {
        p->poly_fb -= num_samples;
        if (p->poly_fb <= 0)
            p->poly_fb_due = true; /* deadline passed unanswered */
    }
    if (p->poly_fill >= hop
        && p->in_write >= (long long) p->polyf0.win_size)
    {
        p->poly_fill = 0;
        const long long start = p->in_write - p->polyf0.win_size;
        for (int i = 0; i < p->polyf0.win_size; ++i)
            p->frame[i] = p->in_buf[(start + i) & p->buf_mask];
        /* The tracker's energy gate mirrors the engine's own (an absolute
           gate left a 20 dB dead zone where the engine was voiced and the
           tracker mute), and births confirm on a single sighting while
           the onset burst vouches for a physical attack. */
        p->polyf0.gate_rms   = GATE (p);
        p->polyf0.fast_birth = p->poly_burst > 0;
        ae_polyf0_process (&p->polyf0, p->frame);

        const double ref = p->ref_hz > 0.0 ? p->ref_hz
                                           : AE_REFERENCE_C0_HZ;
        const double base_level = body_level_now (p);
        /* If the answer deadline just passed but a SUBSTANTIAL birth fires
           this very frame, the onset was that new note (a hammer-on adding
           to the ring, a late-confirming chord tone) -- stand the fallback
           down rather than re-strike the whole ring on top of it. The
           level bar matters: a weak birth (a transient remnant that slipped
           the harmonicity gate) must NOT count as the onset's answer, or a
           ghost blip eats the re-strum. */
        if (p->poly_fb_due && chord_sampler)
            for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
                if (p->polyf0.notes[k].active && k < p->poly_cap
                    && p->polyf0.notes[k].id != p->poly_prev_id[k]
                    && p->polyf0.notes[k].level >= 0.35)
                { p->poly_fb_due = false; break; }
        int active = 0;
        for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
        {
            const AePolyNote *note = &p->polyf0.notes[k];
            /* Export every tracked note; polyNotes caps the SAMPLER. */
            long deg = 0;
            if (note->active)
            {
                const AeTuningResult q = ae_quantize_to_edo_scale_ex (
                    note->hz, p->edo, p->enabled_deg, ref, period);
                deg = q.degree;
            }
            atomic_store_explicit (&p->poly_note_out[k],
                    note->active ? ae_poly_note_pack ((float) note->hz,
                                                      (int) deg,
                                                      note->level, note->id)
                                 : 0,
                    memory_order_relaxed);

            const bool on = note->active && k < p->poly_cap;
            if (on)
                ++active;
            if (! chord_sampler)
            {
                /* No bank striking: keep the row's history clean so a
                   mid-chord source flip strikes what is still sounding. */
                p->poly_prev_id[k] = -1;
                continue;
            }
            double out_hz = 0.0;
            if (on)
                /* the snapped degree plus the static lead transpose --
                   the corrective Mellotron */
                out_hz = ae_degree_hz (deg + p->lead_shift,
                                       p->edo, ref, period);
            /* A chord member's share of the strum: salience-relative,
               COMPRESSED (sqrt) -- raw relative salience spread members
               by up to 14 dB, which the ear read as notes randomly
               missing from the chord. sqrt halves the dB spread; the
               verdict's refinement preserves it via the slot's wrel. */
            const double w_note = on ? sqrt (note->level) : 0.0;
            if (on && note->id != p->poly_prev_id[k])
            {
                /* birth: a new note on this row strikes fresh -- and
                   owns the row for this onset (no restrike on top) */
                sample_strike (p, k, out_hz,
                               strike_gain (p, base_level, note->hz)
                                   * w_note,
                               w_note);
                /* bend-follow centre, seeded at birth: the note STARTS
                   exactly corrected; deviation from here is expression */
                p->poly_dev_ema[k] = 1200.0
                    * log2 (note->hz / ae_degree_hz (deg, p->edo, ref, period));
                if (note->level >= 0.35)
                {
                    /* a substantial new note answers the onset; a weak
                       one (transient remnant) must not eat the re-strum */
                    p->poly_fb = 0;
                    p->poly_fb_due = false;
                }
                /* A superseded note that only sounded a moment is a
                   CORRECTION (the onset burst's early birth settling on
                   its true pitch), not a note the player meant to let
                   ring: crossfade it out in 6 ms instead of letting a
                   wrong degree ring through the release. Mature notes
                   keep their let-ring. Age in output samples = pos/rate. */
                for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
                    if (p->smp[k][sl].rec != NULL && p->smp[k][sl].releasing
                        && p->smp[k][sl].rate > 0.0
                        && p->smp[k][sl].pos
                               < 0.12 * p->fs * p->smp[k][sl].rate)
                    {
                        p->smp[k][sl].releasing = false;
                        p->smp[k][sl].retiring  = true;
                    }
                p->poly_prev_id[k] = note->id;
                p->poly_refired[k] = true;
            }
            else if (on && p->poly_restrike > 0 && ! p->poly_refired[k]
                     && note->raw > 1.15 * p->poly_base_raw[k])
            {
                /* re-pluck: the note never died (same id), but an onset
                   landed and THIS note's own salience rose past its
                   pre-pluck baseline -- the player hit the string again.
                   Without this a re-strum of a ringing chord is silent.
                   The raw (un-normalised) salience is the tell; the
                   relative level cannot be, because a new loud note
                   renormalises everyone. 1.15x: a modest rise is enough
                   -- the Hann window centre-weights old audio, so even a
                   hard re-pluck under a loud ring shows as a small jump
                   at first (1.3x sat out too many real re-strums; the
                   ones it still misses fall to the deadline fallback
                   below). */
                sample_strike (p, k, out_hz,
                               strike_gain (p, base_level, note->hz)
                                   * w_note,
                               w_note);
                p->poly_refired[k] = true;
                p->poly_fb = 0; /* the onset is answered */
                p->poly_fb_due = false;
            }
            else if (on && p->poly_fb_due && ! p->poly_refired[k])
            {
                /* The answer deadline passed with the onset unattributed:
                   the player audibly attacked and nothing struck -- the
                   re-strum case a still-loud ring hides from the salience
                   test. Re-strike what is provably sounding; the pitch is
                   already validated, so the worst case is a doubled pad
                   swell, never a wrong note. (The field failure this
                   answers: "very finicky and randomly produces no
                   output".) */
                sample_strike (p, k, out_hz,
                               strike_gain (p, base_level, note->hz)
                                   * w_note,
                               w_note);
                p->poly_refired[k] = true;
            }
            else if (on)
            {
                /* Physical BEND-FOLLOW (research batch, "correct the
                   centre, keep the modulation"): the note's deviation from
                   its snapped degree rides the sample rate, scaled by
                   EXPRESS, minus a slow per-row centre (~0.7 s) -- a bend
                   or vibrato passes through continuously while a sustained
                   detune is still corrected away. Clamped to one EDO step:
                   past that the tracker re-snaps the degree anyway, and a
                   glitched frame must not fling the pitch. */
                const double step_c = period / (double) (p->edo > 0 ? p->edo : 12);
                double dev = 1200.0
                    * log2 (note->hz / ae_degree_hz (deg, p->edo, ref, period));
                p->poly_dev_ema[k] += (dev - p->poly_dev_ema[k]) * 0.03;
                double bend = (dev - p->poly_dev_ema[k]) * p->expression;
                if (bend >  step_c) bend =  step_c;
                if (bend < -step_c) bend = -step_c;
                sample_repitch (p, k, out_hz * pow (2.0, bend / 1200.0));
            }
            else if (p->poly_prev_id[k] != -1)
            {
                /* death: the row's slots decay under the release
                   ceiling (leadReleaseMs), exactly like a superseded
                   let-ring note -- "cutting off when notes stop" at
                   the pace the player chose. A note that only sounded
                   a moment before dying was the onset burst's early
                   guess, not a note: 6 ms crossfade, same rule as the
                   birth correction above. */
                for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
                    if (p->smp[k][sl].rec != NULL)
                    {
                        if (p->smp[k][sl].rate > 0.0
                            && p->smp[k][sl].pos
                                   < 0.12 * p->fs * p->smp[k][sl].rate)
                            p->smp[k][sl].retiring = true;
                        else
                            p->smp[k][sl].releasing = true;
                    }
                p->poly_prev_id[k] = -1;
            }
        }
        p->poly_fb_due = false; /* the fallback fires on one frame only */
        atomic_store_explicit (&p->poly_active_out, active,
                               memory_order_relaxed);
    }

    /* A sample lead no longer layers the SHIFTED chord under itself --
       with a bank loaded or not: the mix's other side is the latency-free
       dry (below), which covers the onset better than a delayed shift
       ever did.

       The MEL layer (experimental, the EHX 9-series move) is the OTHER
       use of the shifter this mode frees up: pointed one clean octave up
       (a 2:1 harmonic ratio stays in tune in ANY tuning -- no grid in the
       path), its output is stacked with the latency-matched dry at
       delivery below, under the layer's own swell. Zero decisions live
       in it: when the tracker drops a note mid-chord, this layer still
       carries it, so detection errors degrade as timbre, never as
       missing or wrong notes. The sampler's fast strikes cover the
       attack; the swell blooms the transform in to own the sustain --
       which also makes the shifter's poly latency read as intent. */
    set_shift (p->shifter, lead_st, 220.0, p->formant_hold, p->formant_st);
    if (p->lead_source != AE_HARM_SRC_SAMPLE)
    {
        if (! p->poly_shift_fed)
        {
            /* the shifter sat unfed while the sampler ran; its state is
               stale audio -- start clean */
            ae_shifter_reset (p->shifter);
            p->poly_shift_fed = true;
        }
        ae_shifter_process (p->shifter, p->in_block, p->wet_buf, num_samples);
    }
    else
    {
        /* Sample lead, bank loaded OR NOT: the wet slot stays silent.
           With a bank the sampler rows own the strike; without one the
           mix's latency-free dry side alone carries the instrument --
           mono's rule exactly.  Running the shifted (unison) chord under
           that same dry summed two copies of one signal ~100 ms apart:
           a comb that measured -2 dB overall with the top octaves nulled
           (field: "quieter than with autoedo off... missing high end"). */
        p->poly_shift_fed = false;
        memset (p->wet_buf, 0, (size_t) num_samples * sizeof (float));
    }

    /* The MEL remodeller: up to three resynthesized copies of the input
       spectrum through the dedicated bank -- including the UNISON copy,
       whose resynthesis smears the pick transient (the single strongest
       "this is a guitar" cue) -- each under its own spectral shape, the
       sum under shared tape wow/flutter. Saturation and the bandwidth
       rolloff run in a second pass; the swell scales it at delivery. */
    const AeMelPresetSpec *mp = mel_spec (p);
    /* POLY is the only requirement (field: "turned on Poly but the Mel
       control remained grayed") -- with its own shifter bank the layer no
       longer borrows anything from the chord sampler, so it serves as the
       pure 9-series voice with no bank loaded at all, or layers under the
       sampler when one is up. */
    const bool mel_on = p->mel_mix > 0.001
                     && p->mel_shifter[0] != NULL && p->mel_sum != NULL;
    if (mel_on)
    {
        if (! p->mel_fed)
        {
            for (int l = 0; l < 3; ++l)
            {
                ae_shifter_reset (p->mel_shifter[l]);
                p->mel_lp[l] = 0.0;
            }
            p->mel_mlp = 0.0;
            p->mel_fed = true;
        }
        /* One tape, one capstan: a single wow walk detunes every layer
           together, plus a faster flutter. Per block is plenty. */
        p->mel_rng = p->mel_rng * 1664525u + 1013904223u;
        const double r01 = (double) (p->mel_rng >> 8) / 16777216.0;
        p->mel_wow += (r01 - 0.5) * mp->wow_c * 0.2;
        if (p->mel_wow >  mp->wow_c) p->mel_wow =  mp->wow_c;
        if (p->mel_wow < -mp->wow_c) p->mel_wow = -mp->wow_c;
        p->mel_flut_ph += 6.3 * 2.0 * M_PI * num_samples / p->fs;
        if (p->mel_flut_ph > 2.0 * M_PI)
            p->mel_flut_ph -= 2.0 * M_PI;
        const double wob_st = (p->mel_wow
                               + mp->flut_c * sin (p->mel_flut_ph)) / 100.0;

        memset (p->mel_sum, 0, (size_t) num_samples * sizeof (float));
        const double env_now = p->mel_env; /* for the brass filter sweep */
        for (int l = 0; l < 3; ++l)
        {
            const double g = mp->layer[l].gain < 0.0 ? p->mel_oct_g
                                                     : mp->layer[l].gain;
            if (g <= 0.001)
                continue;
            set_shift (p->mel_shifter[l],
                       lead_st + mp->layer[l].st + wob_st, 220.0,
                       p->formant_hold, p->formant_st);
            /* voice_buf is free until the ghosts run (after delivery) */
            ae_shifter_process (p->mel_shifter[l], p->in_block,
                                p->voice_buf, num_samples);
            double fc = mp->layer[l].lp_hz;
            if (mp->layer[l].sweep)
                /* the brass move: the swell IS a filter sweep -- the
                   spectral trajectory, not just a fade */
                fc *= 0.2 + 0.8 * env_now;
            if (fc > 0.0)
            {
                const double a = 1.0 - exp (-2.0 * M_PI * fc / p->fs);
                double s = p->mel_lp[l];
                for (int i = 0; i < num_samples; ++i)
                {
                    s += ((double) p->voice_buf[i] - s) * a;
                    p->mel_sum[i] += (float) (g * s);
                }
                p->mel_lp[l] = s;
            }
            else
                for (int i = 0; i < num_samples; ++i)
                    p->mel_sum[i] += (float) (g * p->voice_buf[i]);
        }
        /* Tape character on the sum: soft saturation, then the bandwidth
           rolloff -- the deliberate Mellotron ugliness, kept modest. */
        if (mp->drive > 0.0 || mp->master_lp > 0.0)
        {
            const double d = mp->drive;
            const double a = mp->master_lp > 0.0
                ? 1.0 - exp (-2.0 * M_PI * mp->master_lp / p->fs) : 1.0;
            double s = p->mel_mlp;
            for (int i = 0; i < num_samples; ++i)
            {
                double x = p->mel_sum[i];
                if (d > 0.0)
                    x = x * (1.0 + d) / (1.0 + d * fabs (x));
                s += (x - s) * a;
                p->mel_sum[i] = (float) s;
            }
            p->mel_mlp = s;
        }
    }
    else
        p->mel_fed = false;
    /* STEEL in poly: the drone sits under the LOWEST tracked note --
       polyphony only has to prove a note exists; the drone is always the
       root. Sample drone with the chord sampler, synth drone otherwise. */
    {
        long low = AE_HARM_DEG_OFF;
        for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
        {
            const uint64_t w = atomic_load_explicit (&p->poly_note_out[k],
                                                     memory_order_relaxed);
            if (ae_poly_note_hz (w) > 0.0f)
            {
                const long d = (long) ae_poly_note_deg (w) + p->lead_shift;
                if (low == AE_HARM_DEG_OFF || d < low)
                    low = d;
            }
        }
        steel_process (p, gate, low, chord_sampler, num_samples);
    }
    if (chord_sampler)
    {
        const double lr = p->lead_release_ms > 5.0 ? p->lead_release_ms : 5.0;
        const double l_rel_a = 1.0 - exp (-1.0 / (lr * 0.001 * p->fs));
        const double fade_step = 1.0 / (0.006 * p->fs);
        for (int i = 0; i < num_samples; ++i)
        {
            double x = 0.0;
            for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
                x += sample_mix_slots (p, k, fade_step, l_rel_a);
            x = sample_tone_tick (p, AE_HARM_VOICES, x);
            p->wet_buf[i] = (float) (x * g_smp_lead * p->smp_level);
        }
    }

    const double gain_alpha = 1.0 - exp (-1.0 / (0.005 * p->fs));
    const double la_ms = p->lead_attack_ms  > 5.0 ? p->lead_attack_ms  : 5.0;
    const double lr_ms = p->lead_release_ms > 5.0 ? p->lead_release_ms : 5.0;
    const double l_atk = 1.0 - exp (-1.0 / (la_ms * 0.001 * p->fs));
    const double l_rel = 1.0 - exp (-1.0 / (lr_ms * 0.001 * p->fs));
    const double f_lvl = (double) atomic_load_explicit (
        &p->follow_level_in, memory_order_relaxed);
    const double f_tgt = (1.0 - p->follow_amt) + p->follow_amt * f_lvl;
    const double f_a   = 1.0 - exp (-1.0 / (0.010 * p->fs));
    /* The MEL layer's own envelope: the swell is the instrument identity
       (an onset that BLOOMS reads as bowed/brass/tape, and buries the
       shifter's poly latency); the release is a tail ceiling like the
       lead's. Gated on energy alone -- no decisions. */
    const double m_atk = 1.0 - exp (-1.0 /
        ((p->mel_atk_ms > 5.0 ? p->mel_atk_ms : 5.0) * 0.001 * p->fs));
    const double m_rel = 1.0 - exp (-1.0 /
        ((p->mel_rel_ms > 5.0 ? p->mel_rel_ms : 5.0) * 0.001 * p->fs));
    double m_env = p->mel_env;
    double v_gain = p->v_gain, l_env = p->lead_env, f_g = p->follow_gain_cur;
    const long long block_start = p->in_write - num_samples;
    for (int i = 0; i < num_samples; ++i)
    {
        const double target = gate ? 1.0 : 0.0;
        v_gain += (target - v_gain) * gain_alpha;
        l_env  += (target - l_env) * (gate ? l_atk : l_rel);
        f_g    += (f_tgt - f_g) * f_a;
        m_env  += (target - m_env) * (gate ? m_atk : m_rel);
        const long long t_out = block_start + i - p->latency;
        const float dry = t_out < 0 ? 0.0f : p->in_buf[t_out & p->buf_mask];
        const double wet_only = v_gain * l_env * f_g * p->wet_buf[i];
        if (wet_out != NULL)
            wet_out[i] = (float) wet_only;
        /* The sample mix's dry side: the CURRENT input block, latency-free
           -- the hands wait on the hardware buffer and nothing else. It
           replaces the latency-matched unvoiced fallback for a sample
           lead (both at once would double the instrument). */
        p->smp_dry_g += (g_dry_lead - p->smp_dry_g) * gain_alpha;
        mono[i] = (float) ((p->lead_gain > 0.0 ? p->lead_gain : 1.0)
                               * (wet_only
                                  + (p->lead_source == AE_HARM_SRC_SAMPLE
                                         ? 0.0
                                         : (1.0 - v_gain) * dry))
                           + p->smp_dry_g * p->in_block[i]
                           /* the MEL layer: the preset's share of raw dry
                              (footage only) plus the remodelled sum --
                              everything at the shifter's latency, mutually
                              phase-honest -- under the swell */
                           + (mel_on
                                  ? p->mel_mix * m_env
                                        * (mp->g_dry * dry + p->mel_sum[i])
                                  : 0.0)
                           + p->steel_buf[i]);
    }
    p->v_gain = v_gain; p->lead_env = l_env; p->follow_gain_cur = f_g;
    p->mel_env = m_env;

    if (p->ir_lead != NULL)
        irc_point_process (p->ir_lead, mono, mono, num_samples);

    if (harm_l == NULL)
        return;
    for (int i = 0; i < num_samples; ++i)
    {
        harm_l[i] = 0.0f;
        harm_r[i] = 0.0f;
    }

    /* Ghosts: fixed intervals on the chord. Same envelope, gains and pans
       as the mono path; the interval maths is the voice's own (steps +
       octave extension in the interval's direction + detune), with no
       lock/mask -- there is no detected root to lock TO. */
    double h_atk, h_rel;
    harm_env_coeffs (p, &h_atk, &h_rel);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        const bool configured = p->harm_on && ae_harm_voice_on (&p->harm[v])
                              && p->h_shifter[v] != NULL;
        if (! configured)
        {
            p->h_fed[v] = false;
            p->h_mix[v] = 0.0;
            continue;
        }
        if (! p->h_fed[v])
        {
            ae_shifter_reset (p->h_shifter[v]);
            p->h_fed[v] = true;
            p->h_mix[v] = 0.0;
        }
        const int dir = p->harm[v].interval >= 0 ? 1 : -1;
        const double steps = (double) p->harm[v].interval
                           + (double) (p->harm[v].ext_oct * dir * p->edo);
        const double st = (steps * step_c + p->harm[v].detune_cents) / 100.0;
        set_shift (p->h_shifter[v], st, 220.0, p->formant_hold, p->formant_st);
        ae_shifter_process (p->h_shifter[v], p->in_block, p->voice_buf,
                            num_samples);
        const bool   vgate = gate && ! p->harm[v].mute;
        const double want  = vgate ? 1.0 : 0.0;
        const double a     = vgate ? h_atk : h_rel;
        const double gl = p->h_gl[v], gr = p->h_gr[v];
        double mix = p->h_mix[v];
        for (int i = 0; i < num_samples; ++i)
        {
            mix += (want - mix) * a;
            const double sv = p->voice_buf[i] * mix;
            harm_l[i] += (float) (gl * sv);
            harm_r[i] += (float) (gr * sv);
        }
        p->h_mix[v] = mix;
        atomic_store_explicit (&p->h_deg_out[v],
                               vgate ? (int) steps : AE_HARM_DEG_OFF,
                               memory_order_relaxed);
    }

    if (p->ir_harm[0] != NULL)
    {
        irc_point_process (p->ir_harm[0], harm_l, harm_l, num_samples);
        irc_point_process (p->ir_harm[1], harm_r, harm_r, num_samples);
    }
    if (p->harm_tilt_db != 0.0)
        render_tilt (p, harm_l, harm_r, num_samples);
    apply_harm_master (p, harm_l, harm_r, num_samples);
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
        const double vel = strike_gain (p, body_level_now (p),
                                        (double) ae_corrector_detected_hz (p));
        const int L = AE_HARM_VOICES;
        if (p->smp_guard > 0)
            p->smp_guard -= num_samples;
        if (p->onset_pulse)
        {
            p->smp_pending[L] = true;
            /* 400 ms, doubled: a post-silence OCTAVE change can hold the
               detector's vote past 200, and a window that expires before
               the pitch lands eats the note outright. */
            p->smp_wait[L] = (int) (0.40 * p->fs);
        }
        if (p->smp_pending[L])
        {
            p->smp_wait[L] -= num_samples;
            if (p->smp_wait[L] <= 0)
                p->smp_pending[L] = false;
            else if (hz > 0.0)
            {
                sample_strike (p, L, hz, vel, 1.0);
                p->smp_pending[L] = false;
                p->smp_guard = (int) (0.040 * p->fs);
            }
        }

        /* The energy Schmitt is NOT the only note edge -- it misses
           re-plucks under a still-ringing string (small fast/slow
           contrast) and legato notes that never dip at all, which played
           as "losing repeated and successive notes" on a sampled lead.
           Two more triggers, one guard against double-firing:
           - the corrected DEGREE changed while voiced: a hammer-on, a
             run, or a pick the Schmitt slept through -- a new note is a
             new strike;
           - the attack hold ARMED (energy onset, pitch jump, re-voice
             after a pick's dip) while the envelope actually rose
             (fast > 1.3x slow): a re-pluck of the SAME note. The
             envelope condition keeps deep vibrato -- which can arm the
             hold -- from machine-gunning the sample. */
        {
            const int ldeg = atomic_load_explicit (&p->lead_deg_out,
                                                   memory_order_relaxed);
            bool want = false;
            if (hz > 0.0 && ldeg != AE_HARM_DEG_OFF)
            {
                /* Bend-vs-jump (research batch): a degree change reached by
                   a JUMP is a new note and strikes; one reached by a CREEP
                   is the player bending, and the ringing sample follows it
                   via the continuous repitch below instead of being
                   re-picked at every degree line. The detector smears even
                   an instant legato step across the analysis frame (~5
                   hops), so a semitone hammer-on reads ~20-40 c/hop; a
                   real bend measures under ~6 c/hop even played fast. 15
                   splits them -- the cost is that a ONE-step legato in a
                   fine EDO (54 c in 22) glides rather than re-picks, which
                   is the pedal-steel reading of that gesture anyway. */
                {
                    const long long dstep =
                        (long long) ldeg - (long long) p->smp_lead_deg;
                    const long long half_oct =
                        (long long) ((p->edo > 0 ? p->edo : 12) / 2);
                    /* The slope gate is a claim about BENDS, and a bend
                       cannot creep half an octave inside one hop -- an
                       octave-scale degree change is a new note however
                       steady the pitch reads (field: "play a note, stop,
                       play an octave higher -- it gets eaten": the
                       detector re-locks CLEAN on the new octave, the
                       slope reads zero, and the gate swallowed the
                       strike). */
                    if (p->smp_lead_deg_valid && ldeg != (long long) p->smp_lead_deg
                        && ! p->rel_collapse
                        && (p->det_slope_hold > 15.0
                            || llabs (dstep) >= half_oct))
                        want = true;
                }
                if (p->att_seq != p->att_seq_seen
                    && p->atk_fast > 1.3 * p->atk_slow)
                    want = true;
                p->smp_lead_deg       = ldeg;
                p->smp_lead_deg_valid = true;
            }
            else
                p->smp_lead_deg_valid = false;
            p->att_seq_seen = p->att_seq;
            if (want && p->smp_guard <= 0 && hz > 0.0)
            {
                sample_strike (p, L, hz, vel, 1.0);
                p->smp_pending[L] = false; /* this event is served */
                p->smp_guard = (int) (0.040 * p->fs);
            }
        }
        /* Continuous repitch: the corrected pitch WITH the playing's own
           deviation (EXPRESS) on top -- a physical bend rides the
           recording. Falls back to the snapped target on builds/paths
           that have not published a corrected pitch this block. */
        {
            const double ch = (double) atomic_load_explicit (
                &p->corr_hz_out, memory_order_relaxed);
            sample_repitch (p, L, ch > 0.0 ? ch : hz);
        }
        const double fade_step = 1.0 / (0.006 * p->fs);
        /* The LEAD's ceiling is its own release, not the harmony's. */
        const double lr = p->lead_release_ms > 5.0 ? p->lead_release_ms : 5.0;
        const double l_rel = 1.0 - exp (-1.0 / (lr * 0.001 * p->fs));
        /* The consolidated sample section: the mix's sample side and the
           engine-wide sample level, applied at render so a fader move
           reaches notes already ringing; tone on the summed row. The dry
           side is mixed at delivery (section 3), latency-free. */
        double g_dry_l, g_smp_l;
        sample_layer_gains (p->smp_mix, &g_dry_l, &g_smp_l);
        (void) g_dry_l;
        const double g_l = g_smp_l * p->smp_level;
        for (int i = 0; i < num_samples; ++i)
        {
            double x = sample_mix_slots (p, L, fade_step, l_rel);
            x = sample_tone_tick (p, AE_HARM_VOICES, x);
            p->wet_buf[i] = (float) (x * g_l);
        }
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
           point of pointing it at the lead. Then the consolidated
           SYNTH/SAMPLE section: fader, mix taper and tone, the same law
           the sample lead wears (the synth lead had NO level control after
           leadSoundGainDb retired -- the "not working or very quiet"). */
        double g_dry_s, g_smp_s;
        sample_layer_gains (p->smp_mix, &g_dry_s, &g_smp_s);
        (void) g_dry_s; /* the dry side is mixed at delivery, latency-free */
        const double match = dclamp (p->in_level / synth_patch_rms (pat),
                                     0.0, 4.0)
                           * p->smp_level * g_smp_s;
        for (int i = 0; i < num_samples; ++i)
            p->wet_buf[i] = (float) (sample_tone_tick (p, AE_HARM_VOICES,
                                                       p->wet_buf[i])
                                     * match);
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

    /* The STEEL drone rides under whatever the lead is (its render is a
       no-op while the mode is off and nothing rings). */
    steel_process (p, p->voiced,
                   (long) atomic_load_explicit (&p->lead_deg_out,
                                                memory_order_relaxed),
                   lead_sample, num_samples);

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

    /* FOLLOW gain: g = (1 - depth) + depth * level, smoothed ~10 ms. At
       depth 1 the source's envelope IS this voice's envelope -- the
       source going silent cuts the voice, which is the point. Applied to
       the WET lead here and to the harmony bus in apply_harm_master; the
       latency-matched dry path is not touched (it only sounds while
       unvoiced). */
    const double f_lvl = (double) atomic_load_explicit (
        &p->follow_level_in, memory_order_relaxed);
    const double f_tgt = (1.0 - p->follow_amt) + p->follow_amt * f_lvl;
    const double f_a   = 1.0 - exp (-1.0 / (0.010 * p->fs));
    double f_g = p->follow_gain_cur;

    /* The mix's DRY side (a sample OR synth lead): the CURRENT input
       block at the taper's dry gain -- the hands wait on the hardware
       buffer and nothing else, never on the detector's pipeline. By
       config INTENT, not bank state: a library that failed to load must
       not silence the player. */
    double g_dry_lead = 0.0;
    if (lead_sample || lead_synth)
    {
        double g_smp_unused;
        sample_layer_gains (p->smp_mix, &g_dry_lead, &g_smp_unused);
        (void) g_smp_unused;
    }
    for (int i = 0; i < num_samples; ++i)
    {
        const double target = p->voiced ? 1.0 : 0.0;
        v_gain += (target - v_gain) * gain_alpha;
        l_env  += (target - l_env) * (p->voiced ? l_atk : l_rel);

        const long long t_out = block_start + i - p->latency;
        const float dry = (no_dry || t_out < 0)
                              ? 0.0f : p->in_buf[t_out & p->buf_mask];
        f_g += (f_tgt - f_g) * f_a;
        const double wet_g = (no_dry ? l_env : v_gain * l_env) * f_g;
        const double wet_only = wet_g * p->wet_buf[i];
        if (wet_out != NULL)
            wet_out[i] = (float) wet_only;
        p->smp_dry_g += (g_dry_lead - p->smp_dry_g) * gain_alpha;
        /* leadGainDb scales the LEAD VOICE (wet + its unvoiced fallback)
           and nothing else: the mix's dry side, the steel drone and (in
           poly) the MEL layer have their own faders and must not die
           with this one. */
        mono[i] = (float) ((p->lead_gain > 0.0 ? p->lead_gain : 1.0)
                               * (wet_only + (1.0 - v_gain) * dry)
                           + p->smp_dry_g * p->in_block[i]
                           + p->steel_buf[i]);
    }
    p->v_gain = v_gain;
    p->lead_env = l_env;
    p->follow_gain_cur = f_g;

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
            /* A sample voice is a sample voice: the mix's other side is
               the latency-free dry at delivery, not a shifted copy, so
               the shifter stays parked. */
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
        if (p->poly)
            process_poly (p, mono + done,
                       harm_l != NULL ? harm_l + done : NULL,
                       harm_r != NULL ? harm_r + done : NULL,
                       p->lead_wet != NULL && done + m <= p->max_block
                           ? p->lead_wet + done : NULL, m);
        else
            process_chunk (p, mono + done,
                       harm_l != NULL ? harm_l + done : NULL,
                       harm_r != NULL ? harm_r + done : NULL,
                       p->lead_wet != NULL && done + m <= p->max_block
                           ? p->lead_wet + done : NULL, m);
        done += m;
    }
}
