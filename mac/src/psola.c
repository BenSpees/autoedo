#include "psola.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AE_PI       3.14159265358979323846
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

void ae_psola_prepare (AePsola *p, double sample_rate, int max_block_size,
                       double min_hz, double max_hz)
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

    /* Latency must guarantee that every delivered output sample is fully
       covered by already-placed grains: 3 * longest period. */
    p->latency = 3 * p->tau_max;

    const int need = p->latency + p->max_block + 2 * p->tau_max + p->frame_size + 16;
    p->buf_size = 1;
    while (p->buf_size < need)
        p->buf_size <<= 1;
    p->buf_mask = p->buf_size - 1;

    free (p->in_buf);
    free (p->wet_acc);
    free (p->wet_win);
    free (p->frame);
    p->in_buf  = calloc ((size_t) p->buf_size, sizeof (float));
    p->wet_acc = calloc ((size_t) p->buf_size, sizeof (float));
    p->wet_win = calloc ((size_t) p->buf_size, sizeof (float));
    p->frame   = calloc ((size_t) p->frame_size, sizeof (float));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        free (p->h_acc[v]);
        free (p->h_win[v]);
        p->h_acc[v] = calloc ((size_t) p->buf_size, sizeof (float));
        p->h_win[v] = calloc ((size_t) p->buf_size, sizeof (float));
    }

    ae_yin_prepare (&p->detector, p->fs, p->frame_size, min_hz, max_hz);

    if (p->edo == 0) /* first prepare on a zeroed struct: neutral defaults */
    {
        p->edo           = 12;
        p->retune_ms     = 20.0;
        p->transition_ms = 50.0;
        p->amount        = 1.0;
        p->ref_hz        = AE_REFERENCE_C0_HZ;
        p->period_cents  = 1200.0;
    }

    ae_psola_reset (p);
}

void ae_psola_reset (AePsola *p)
{
    memset (p->in_buf,  0, (size_t) p->buf_size * sizeof (float));
    memset (p->wet_acc, 0, (size_t) p->buf_size * sizeof (float));
    memset (p->wet_win, 0, (size_t) p->buf_size * sizeof (float));

    p->in_write       = 0;
    p->last_detect_at = 0;
    p->last_touched   = -1;
    p->synth_mark     = (double) p->tau_max;

    p->current_period = dclamp (p->fs / 220.0, (double) p->tau_min, (double) p->tau_max);
    p->synth_period   = p->current_period;
    p->voiced         = false;
    p->primed         = false;
    p->out_cents      = 0.0;
    p->v_gain         = 0.0;
    p->target_j       = 0;
    p->target_valid   = false;
    p->in_transition  = false;
    p->sustain_s      = 0.0;

    for (int i = 0; i < AE_MAX_EDO; ++i)
        p->enabled_deg[i] = true; /* default: every degree usable */

    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        if (p->h_acc[v] != NULL)
            memset (p->h_acc[v], 0, (size_t) p->buf_size * sizeof (float));
        if (p->h_win[v] != NULL)
            memset (p->h_win[v], 0, (size_t) p->buf_size * sizeof (float));
        p->h_mark[v]    = 0.0;
        p->h_touched[v] = -1;
        p->h_period[v]  = p->current_period;
        p->h_active[v]  = false;
        atomic_store_explicit (&p->h_deg_out[v], AE_HARM_DEG_OFF, memory_order_relaxed);
    }

    atomic_store_explicit (&p->detected_hz_out, 0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->target_hz_out,   0.0f, memory_order_relaxed);
    atomic_store_explicit (&p->voiced_out,      false, memory_order_relaxed);
}

void ae_psola_free (AePsola *p)
{
    free (p->in_buf);
    free (p->wet_acc);
    free (p->wet_win);
    free (p->frame);
    p->in_buf = p->wet_acc = p->wet_win = p->frame = NULL;
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        free (p->h_acc[v]);
        free (p->h_win[v]);
        p->h_acc[v] = p->h_win[v] = NULL;
    }
    ae_yin_free (&p->detector);
}

void ae_psola_set_harmony (AePsola *p, bool on, int lock,
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
        /* constant-power pan */
        const double a = (hv.pan + 1.0) * (AE_PI / 4.0);
        p->h_gl[v] = hv.gain * cos (a);
        p->h_gr[v] = hv.gain * sin (a);
    }
}

static void run_detection (AePsola *p)
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

    if (now_voiced)
    {
        p->current_period = dclamp (p->fs / res.frequency_hz,
                                    (double) p->tau_min, (double) p->tau_max);

        const double ref    = p->ref_hz > 0.0 ? p->ref_hz : AE_REFERENCE_C0_HZ;
        const double period = p->period_cents > 0.0 ? p->period_cents : 1200.0;

        const double detected_cents = 1200.0 * log2 (res.frequency_hz / ref);
        const double steps          = ae_steps_from_ref (res.frequency_hz, p->edo, ref, period);

        AeTuningResult t = ae_quantize_to_edo_scale_ex (res.frequency_hz, p->edo,
                                                        p->enabled_deg, ref, period);
        long long cand = t.degree;

        /* Stickiness (hysteresis): stay on the previous target until the
           detected pitch has travelled past the midpoint toward the new
           candidate by an extra `stickiness` fraction of the half-gap.
           Kills degree flicker when the step is smaller than vibrato. */
        if (p->target_valid && cand != p->target_j && p->stickiness > 0.0)
        {
            const int last_deg = (int) (((p->target_j % p->edo) + p->edo) % p->edo);
            if (p->enabled_deg[last_deg]
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

        double beta = pow (2.0, (p->out_cents - detected_cents) / 1200.0);
        beta = dclamp (beta, 0.5, 2.0); /* safety net; correction ratios stay ~1 */
        p->synth_period = p->current_period / beta;

        p->primed = true;

        /* ---- harmony voices (Xentar emulation) --------------------------- */
        /* Source = the corrected target degree; voices ride the same glide. */
        bool any_solo = false;
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            if (p->harm[v].solo && p->harm[v].interval != 0 && ! p->harm[v].mute)
                any_solo = true;

        double used_cents[AE_HARM_VOICES];
        int    used_n = 0;

        for (int v = 0; v < AE_HARM_VOICES; ++v)
        {
            const AeHarmVoice *hv = &p->harm[v];
            p->h_active[v] = false;
            int deg_out = AE_HARM_DEG_OFF;

            if (p->harm_on && hv->interval != 0 && ! hv->mute
                && ! (any_solo && ! hv->solo))
            {
                /* eff = interval + sign(interval) * extOct * equaveSteps */
                const int eff = hv->interval
                              + (hv->interval > 0 ? 1 : -1) * hv->ext_oct * p->edo;
                long long gj = cand + eff;
                if (p->harm_lock == 1)
                    gj = ae_walk_to_enabled (gj, p->edo, p->enabled_deg);

                double ghost_cents = (double) gj * period / (double) p->edo;
                if (p->harm_lock == 2)
                    ghost_cents = ji_snap_cents (ghost_cents, period);

                deg_out = (int) lround (ghost_cents * (double) p->edo / period);

                /* Dedupe: voices landing on one pitch must not double up.
                   The deduped voice still reports its degree for the UI. */
                bool dup = false;
                for (int u = 0; u < used_n; ++u)
                    if (fabs (ghost_cents - used_cents[u]) < 0.5)
                        dup = true;

                if (! dup)
                {
                    used_cents[used_n++] = ghost_cents;

                    const double voice_cents = p->out_cents + (ghost_cents - target_cents);
                    double hbeta = pow (2.0, (voice_cents - detected_cents) / 1200.0);
                    /* keep the synthesis period coverable by the grain width */
                    const double beta_min = p->current_period / (double) p->tau_max;
                    hbeta = dclamp (hbeta, beta_min > 0.25 ? beta_min : 0.25, 4.0);
                    p->h_period[v] = p->current_period / hbeta;
                    p->h_active[v] = true;
                }
            }
            atomic_store_explicit (&p->h_deg_out[v], deg_out, memory_order_relaxed);
        }
    }
    else
    {
        p->synth_period = p->current_period; /* identity timeline; crossfade handles the rest */
        p->target_valid = false;
        p->sustain_s    = 0.0;
        for (int v = 0; v < AE_HARM_VOICES; ++v)
        {
            p->h_active[v] = false;
            atomic_store_explicit (&p->h_deg_out[v], AE_HARM_DEG_OFF, memory_order_relaxed);
        }
    }

    p->voiced = now_voiced;
    atomic_store_explicit (&p->voiced_out, now_voiced, memory_order_relaxed);
}

/* Place one Hann grain centred at `center_out` into an accumulator pair.
   `synth_period` sizes the grain so slower (lower-pitched) streams still
   fully overlap. `touched` tracks the highest cleared slot of that pair. */
static void place_grain_into (AePsola *p, float *acc, float *win,
                              long long *touched, long long center_out,
                              double synth_period)
{
    const double T0 = p->current_period;
    const double Tw = synth_period > T0 ? synth_period : T0;
    int half = (int) llround (Tw);
    if (half < p->tau_min) half = p->tau_min;
    if (half > p->tau_max) half = p->tau_max;

    /* Analysis mark: the input-period grid point nearest this output center.
       (Epoch-free placement — a uniform T0 grid; see the plugin source for
       the design discussion.) */
    const long long m        = llround ((double) center_out / T0);
    const long long a_center = llround ((double) m * T0);

    /* Clear any output slots this grain newly reaches, before accumulating. */
    const long long top = center_out + half;
    for (long long s = *touched + 1; s <= top; ++s)
    {
        const int k = (int) (s & p->buf_mask);
        win[k] = 0.0f;
        acc[k] = 0.0f;
    }
    if (top > *touched)
        *touched = top;

    /* Hann-windowed overlap-add. */
    const double norm = 1.0 / (double) (2 * half);
    for (int j = -half; j <= half; ++j)
    {
        const double pos = (double) (j + half) * norm; /* 0..1 */
        const float  wv  = (float) (0.5 - 0.5 * cos (2.0 * AE_PI * pos));

        const long long out_i = center_out + j;
        const long long in_i  = a_center + j;
        const int       ok    = (int) (out_i & p->buf_mask);

        win[ok] += wv;

        float sample = 0.0f;
        if (in_i >= 0 && in_i > p->in_write - p->buf_size && in_i < p->in_write)
            sample = p->in_buf[in_i & p->buf_mask];
        acc[ok] += wv * sample;
    }
}

static void process_chunk (AePsola *p, float *mono, float *harm_l,
                           float *harm_r, int num_samples)
{
    for (int i = 0; i < num_samples; ++i)
    {
        const int w = (int) (p->in_write & p->buf_mask);
        p->in_buf[w] = mono[i];
        ++p->in_write;

        if (! p->primed)
        {
            /* Hold the synthesis pointer at the input frontier until the first
               pitch primes the engine, so a long silent intro doesn't force
               the onset block to "catch up" a backlog of grains. */
            p->synth_mark   = (double) p->in_write;
            p->last_touched = p->in_write - 1;
        }

        if (p->in_write - p->last_detect_at >= p->hop && p->in_write >= p->frame_size)
            run_detection (p);

        const long long frontier = p->in_write - 2 * p->tau_max;
        while (p->primed && p->synth_mark <= (double) frontier)
        {
            place_grain_into (p, p->wet_acc, p->wet_win, &p->last_touched,
                              (long long) llround (p->synth_mark), p->synth_period);
            p->synth_mark += p->synth_period;
        }

        for (int v = 0; v < AE_HARM_VOICES; ++v)
        {
            if (p->h_active[v])
            {
                while (p->h_mark[v] <= (double) frontier)
                {
                    place_grain_into (p, p->h_acc[v], p->h_win[v], &p->h_touched[v],
                                      (long long) llround (p->h_mark[v]), p->h_period[v]);
                    p->h_mark[v] += p->h_period[v];
                }
            }
            else
            {
                /* Idle voices track the frontier and scrub the slot they pass,
                   so reactivation can never replay a stale lap of the ring. */
                p->h_mark[v]    = (double) p->in_write;
                p->h_touched[v] = p->in_write - 1;
                p->h_win[v][w]  = 0.0f;
                p->h_acc[v][w]  = 0.0f;
            }
        }
    }

    /* Deliver: output sample i is the corrected input from `latency` ago. */
    const long long block_start = p->in_write - num_samples;
    const double gain_alpha = 1.0 - exp (-1.0 / (0.005 * p->fs)); /* ~5 ms crossfade */

    for (int i = 0; i < num_samples; ++i)
    {
        const double target = p->voiced ? 1.0 : 0.0;
        p->v_gain += (target - p->v_gain) * gain_alpha;

        const long long t_out = block_start + i - p->latency;
        if (t_out < 0)
        {
            mono[i] = 0.0f;
            if (harm_l != NULL) { harm_l[i] = 0.0f; harm_r[i] = 0.0f; }
            continue;
        }

        const int   idx = (int) (t_out & p->buf_mask);
        const float win = p->wet_win[idx];
        const float dry = p->in_buf[idx];
        const float wet = (win > 1.0e-6f) ? (p->wet_acc[idx] / win) : dry;
        mono[i] = (float) (p->v_gain * wet + (1.0 - p->v_gain) * dry);

        if (harm_l != NULL)
        {
            double hl = 0.0, hr = 0.0;
            for (int v = 0; v < AE_HARM_VOICES; ++v)
            {
                const float hwv = p->h_win[v][idx];
                if (hwv > 1.0e-6f)
                {
                    const double s = p->h_acc[v][idx] / hwv;
                    hl += p->h_gl[v] * s;
                    hr += p->h_gr[v] * s;
                }
            }
            harm_l[i] = (float) (p->v_gain * hl);
            harm_r[i] = (float) (p->v_gain * hr);
        }
    }
}

void ae_psola_process (AePsola *p, float *mono, float *harm_l, float *harm_r,
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
