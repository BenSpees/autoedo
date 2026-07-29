#include "corrector.h"

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
    p->target_j       = 0;
    p->target_valid   = false;
    p->in_transition  = false;
    p->sustain_s      = 0.0;

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
        atomic_store_explicit (&p->h_deg_out[v], AE_HARM_DEG_OFF, memory_order_relaxed);
    }

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
    ae_corrector_free_shifters (p);
    ae_yin_free (&p->detector);
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
        /* constant-power pan */
        const double a = (hv.pan + 1.0) * (AE_PI / 4.0);
        p->h_gl[v] = hv.gain * cos (a);
        p->h_gr[v] = hv.gain * sin (a);
    }
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

                        /* The voice rides the corrected pitch, so its shift is
                           measured from the detected input like the main one. */
                        const double voice_cents = p->out_cents + (ghost_cents - target_cents);
                        p->h_semitones[v] = dclamp ((voice_cents - detected_cents) / 100.0,
                                                    -36.0, 36.0);
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
}

/* Tonality limit for large shifts: frequencies above this are mapped
   non-linearly, which keeps some of the timbre instead of transposing the
   whole spectrum (the poor man's formant preservation — the real thing
   arrives with Signalsmith Stretch 1.3, see shifter.h). Correction-sized
   shifts use a plain linear map, where it buys nothing. */
#define AE_TONALITY_HZ    8000.0
#define AE_TONALITY_MIN_ST 1.0

static void set_shift (AeShifter *s, double semitones)
{
    ae_shifter_set_semitones (s, semitones,
                              fabs (semitones) >= AE_TONALITY_MIN_ST
                                ? AE_TONALITY_HZ : 0.0);
    /* Undo the pitch shift's own formant movement where supported. */
    ae_shifter_set_formant_semitones (s, 0.0, true);
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

    /* 2. Shift. The detection above is centred about half an analysis frame
       behind the newest input, which is close to where the shifter's own
       processing time sits (its input latency), so the ratio lands on the
       audio it was measured from. */
    set_shift (p->shifter, p->shift_semitones);
    ae_shifter_process (p->shifter, p->in_block, p->wet_buf, num_samples);

    /* 3. Deliver the corrected voice against the latency-matched dry path. */
    const long long block_start = p->in_write - num_samples;
    const double gain_alpha = 1.0 - exp (-1.0 / (0.005 * p->fs)); /* ~5 ms crossfade */
    double v_gain = p->v_gain;

    for (int i = 0; i < num_samples; ++i)
    {
        const double target = p->voiced ? 1.0 : 0.0;
        v_gain += (target - v_gain) * gain_alpha;

        const long long t_out = block_start + i - p->latency;
        const float dry = t_out >= 0 ? p->in_buf[t_out & p->buf_mask] : 0.0f;
        mono[i] = (float) (v_gain * p->wet_buf[i] + (1.0 - v_gain) * dry);
    }
    p->v_gain = v_gain;

    if (harm_l == NULL)
        return;

    for (int i = 0; i < num_samples; ++i)
    {
        harm_l[i] = 0.0f;
        harm_r[i] = 0.0f;
    }

    /* 4. Harmony voices: one shifter each, mixed through their own smoothed
       gain so mute/solo and voice changes can't click. A configured voice is
       fed even while muted, so unmuting is instant rather than costing a
       shifter's worth of latency to refill. */
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        const bool configured = p->harm_on && p->harm[v].interval != 0
                              && p->h_shifter[v] != NULL;
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

        set_shift (p->h_shifter[v], p->h_semitones[v]);
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
