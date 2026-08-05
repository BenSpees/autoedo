#include "corrector.h"

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
    p->harm_source = source == AE_HARM_SRC_SYNTH ? AE_HARM_SRC_SYNTH
                                                 : AE_HARM_SRC_VOICE;
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
        p->h_source[v] = (s == AE_HARM_SRC_VOICE || s == AE_HARM_SRC_SYNTH)
                             ? s : AE_HARM_SRC_DEFAULT;
    }
    p->lead_source = lead == AE_HARM_SRC_SYNTH ? AE_HARM_SRC_SYNTH : AE_HARM_SRC_VOICE;
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

    /* Frame must be large enough that frame_size/2 >= the longest period we
       want to detect (= fs / min_hz), so the lowest detectable pitch is
       independent of sample rate. Keep it a power of two. */
    const int longest_period = (int) ceil (p->fs / min_hz);
    p->frame_size = 2048;
    while (p->frame_size < 2 * longest_period)
        p->frame_size <<= 1;
    if (p->frame_size > (1 << 15))
        p->frame_size = 1 << 15;

    /* Detection hop ~5 ms (keeps detection rate bounded at high sample rates). */
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
    free (p->lpc_res);
    p->lpc_res   = calloc ((size_t) p->max_block, sizeof (float));

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
    p->v_gain         = 0.0;
    p->in_level       = 0.0;
    p->target_j       = 0;
    p->target_valid   = false;
    p->in_transition  = false;
    p->sustain_s      = 0.0;

    /* Unity until a controller says otherwise, and STARTING at unity rather
       than ramping to it: a fresh engine must not fade its harmony in. */
    if (p->harm_master <= 0.0)
        p->harm_master = 1.0;
    p->harm_master_cur = p->harm_master;
    p->lead_on         = true;

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
    p->in_buf = p->frame = p->in_block = p->wet_buf = p->voice_buf = NULL;
    free (p->lpc_res);
    p->lpc_res = NULL;
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
        const double steps          = ae_steps_from_ref (res.frequency_hz, p->edo, ref, period);

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
        }
        else
        {
            const AeTuningResult t = ae_quantize_to_edo_scale_ex (res.frequency_hz, p->edo,
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

        atomic_store_explicit (&p->detected_hz_out, (float) res.frequency_hz, memory_order_relaxed);
        atomic_store_explicit (&p->target_hz_out,   (float) target_hz,        memory_order_relaxed);

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
        if (fabs (detected_cents - target_cents) <= p->tolerance_cents)
            eff_cents = detected_cents;
        eff_cents = detected_cents + (eff_cents - detected_cents) * p->amount;

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

        /* The shifter takes semitones. Correction ratios stay near 1; the
           clamp is a safety net against a wild detection. */
        p->shift_semitones = dclamp ((p->out_cents - detected_cents) / 100.0,
                                     -12.0, 12.0);

        p->primed = true;

        /* ---- harmony voices (Xentar emulation) --------------------------- */
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
        const double anchor_cents = p->lead_on ? p->out_cents : detected_cents;

        for (int v = 0; v < AE_HARM_VOICES; ++v)
        {
            const AeHarmVoice *hv = &p->harm[v];
            p->h_active[v] = false;
            int deg_out = AE_HARM_DEG_OFF;

            if (p->harm_on && hv->interval != 0)
            {
                /* eff = interval + sign(interval) * extOct * equaveSteps */
                const int eff = hv->interval
                              + (hv->interval > 0 ? 1 : -1) * hv->ext_oct * p->edo;
                long long gj = cand + eff;
                if (p->harm_lock == 1) /* MIDI notes override the mask here too */
                    gj = ae_walk_to_enabled (gj, p->edo,
                                             midi_active ? held_mask : p->enabled_deg);

                double ghost_cents = (double) gj * period / (double) p->edo;
                if (p->harm_lock == 2)
                    ghost_cents = ji_snap_cents (ghost_cents, period);

                /* Muted / solo-suppressed voices still report their degree so
                   the UI can dim their ruler tick instead of hiding it. */
                deg_out = (int) lround (ghost_cents * (double) p->edo / period);

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
                            anchor_cents + (ghost_cents - target_cents);
                        p->h_semitones[v] = dclamp ((voice_cents - detected_cents) / 100.0,
                                                    -36.0, 36.0);
                        p->h_cents_t[v] = voice_cents; /* synth target pitch */
                        p->h_active[v] = true;
                    }
                }
            }
            atomic_store_explicit (&p->h_deg_out[v], deg_out, memory_order_relaxed);
        }
    }
    else
    {
        p->shift_semitones = 0.0; /* identity; the crossfade handles the rest */
        p->target_valid    = false;
        p->sustain_s       = 0.0;
        for (int v = 0; v < AE_HARM_VOICES; ++v)
        {
            p->h_active[v] = false;
            atomic_store_explicit (&p->h_deg_out[v], AE_HARM_DEG_OFF, memory_order_relaxed);
        }
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
static void set_shift (AeShifter *s, double semitones, double base_hz)
{
    ae_shifter_set_semitones (s, semitones,
                              fabs (semitones) >= AE_TONALITY_MIN_ST
                                ? AE_TONALITY_HZ : 0.0);
    /* Hold the formants still while the pitch moves, so a transposed voice
       still sounds like the same singer instead of chipmunking. */
    ae_shifter_set_formant_semitones (s, 0.0, true);
    ae_shifter_set_formant_base (s, base_hz);
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
    const double match = dclamp (p->in_level / synth_patch_rms (pat), 0.0, 4.0);
    const double atk_a = p->synth_attack_ms <= 0.0 ? 1.0
        : 1.0 - exp (-1.0 / (p->synth_attack_ms * 0.001 * p->fs));
    const double rel_a = p->synth_release_ms <= 0.0 ? 1.0
        : 1.0 - exp (-1.0 / (p->synth_release_ms * 0.001 * p->fs));
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
            continue;
        }

        /* A fresh attack starts on pitch; a sounding voice glides. */
        if (gate && env < 1e-3)
            p->s_cents[v] = p->h_cents_t[v];
        else
            p->s_cents[v] += (p->h_cents_t[v] - p->s_cents[v]) * glide_a;
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
                           float *harm_r, int num_samples)
{
    /* 1. Take in the block: keep a contiguous copy (mono is written in place
       below) and push it into the ring, which feeds detection and the dry
       path. */
    memcpy (p->in_block, mono, (size_t) num_samples * sizeof (float));
    for (int i = 0; i < num_samples; ++i)
    {
        p->in_buf[p->in_write & p->buf_mask] = mono[i];
        ++p->in_write;

        if (p->in_write - p->last_detect_at >= p->hop && p->in_write >= p->frame_size)
            run_detection (p);
    }

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

    const bool lead_synth = p->lead_source == AE_HARM_SRC_SYNTH;

    /* 2. Shift. The detection above is centred about half an analysis frame
       behind the newest input, which is close to where the shifter's own
       processing time sits (its input latency), so the ratio lands on the
       audio it was measured from. A synth lead skips the shifter entirely
       and plays the corrected pitch instead. */
    const double base_hz =
        atomic_load_explicit (&p->detected_hz_out, memory_order_relaxed);
    if (lead_synth)
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
        set_shift (p->shifter, p->shift_semitones, base_hz);
        ae_shifter_process (p->shifter, p->in_block, p->wet_buf, num_samples);
    }

    /* 3. Deliver the corrected voice against the latency-matched dry path.
       A synth lead has no dry component: its "unvoiced" is silence, not the
       raw microphone, which is what makes it a synth lead rather than a
       passthrough with occasional synth. */
    const long long block_start = p->in_write - num_samples;
    const double gain_alpha = 1.0 - exp (-1.0 / (0.005 * p->fs)); /* ~5 ms crossfade */
    double v_gain = p->v_gain;

    for (int i = 0; i < num_samples; ++i)
    {
        const double target = p->voiced ? 1.0 : 0.0;
        v_gain += (target - v_gain) * gain_alpha;

        const long long t_out = block_start + i - p->latency;
        const float dry = (lead_synth || t_out < 0)
                              ? 0.0f : p->in_buf[t_out & p->buf_mask];
        mono[i] = (float) (v_gain * p->wet_buf[i] + (1.0 - v_gain) * dry);
    }
    p->v_gain = v_gain;

    /* The LEAD IR point: on the finished corrected voice. Zero added
       latency by construction (direct first partition) -- this is the live
       monitored path the whole scheme exists for. */
    if (p->ir_lead != NULL)
        irc_point_process (p->ir_lead, mono, mono, num_samples);

    if (harm_l == NULL)
        return;

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
        if (ae_corrector_voice_source (p, v) == AE_HARM_SRC_SYNTH)
            any_synth = true;
        else
            any_shift = true;
    }
    if (any_synth || p->drone_on || p->drone_env >= 1e-4)
        render_synth_harmony (p, harm_l, harm_r, num_samples);
    if (! any_shift)
    {
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
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        const bool configured = p->harm_on && p->harm[v].interval != 0
                              && p->h_shifter[v] != NULL
                              && ae_corrector_voice_source (p, v) == AE_HARM_SRC_VOICE;
        if (! configured)
        {
            p->h_fed[v] = false; /* history is stale; reset before reuse */
            p->h_mix[v] = 0.0;
            continue;
        }

        if (! p->h_fed[v])
        {
            ae_shifter_reset (p->h_shifter[v]);
            p->h_fed[v] = true;
            p->h_mix[v] = 0.0; /* fade in from silence */
        }

        set_shift (p->h_shifter[v], p->h_semitones[v], base_hz);
        ae_shifter_process (p->h_shifter[v], p->in_block, p->voice_buf, num_samples);

        const double want = p->h_active[v] ? 1.0 : 0.0;
        const double gl = p->h_gl[v], gr = p->h_gr[v];
        double mix = p->h_mix[v];
        double vg = p->v_gain;
        for (int i = 0; i < num_samples; ++i)
        {
            mix += (want - mix) * gain_alpha;
            const double s = p->voice_buf[i] * mix * vg;
            harm_l[i] += (float) (gl * s);
            harm_r[i] += (float) (gr * s);
        }
        p->h_mix[v] = mix;
    }

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
                       harm_r != NULL ? harm_r + done : NULL, m);
        done += m;
    }
}
