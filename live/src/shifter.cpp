#include "shifter.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>       /* std::nothrow */
#include <type_traits>
#include <utility>

namespace
{
    using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

    /* Formant control arrived in Signalsmith Stretch 1.3. Detect it rather
       than version-test, so dropping a newer header into third_party/ lights
       it up with no code change. */
    template <typename T>
    struct HasFormants
    {
        template <typename U>
        static auto probe (int)
            -> decltype (std::declval<U &>().setFormantSemitones (0.0f, false),
                         std::true_type());
        template <typename>
        static std::false_type probe (...);

        static const bool value = decltype (probe<T> (0))::value;
    };

    /* Templated so only the branch that matches the vendored header is
       instantiated (a plain overload would be compiled either way). */
    template <typename S>
    inline void apply_formants (S &s, float semitones, bool compensate,
                                std::true_type)
    {
        s.setFormantSemitones (semitones, compensate);
    }
    template <typename S>
    inline void apply_formants (S &, float, bool, std::false_type) {}

    template <typename S>
    inline void apply_formant_base (S &s, float base, std::true_type)
    {
        s.setFormantBase (base);
    }
    template <typename S>
    inline void apply_formant_base (S &, float, std::false_type) {}

    /* splitComputation spreads each block's FFT work across the interval
       instead of doing it in one burst. With six voices whose intervals all
       land on the same sample, that burst is what decides whether the audio
       callback makes its deadline — so it is worth the one extra interval of
       output latency it costs. The 4-argument configure() arrived with the
       same release as the formant methods. */
    template <typename S>
    inline void do_configure (S &s, int block, int interval, std::true_type)
    {
        s.configure (1, block, interval, true);
    }
    template <typename S>
    inline void do_configure (S &s, int block, int interval, std::false_type)
    {
        s.configure (1, block, interval);
    }
}

struct AeShifter
{
    Stretch stretch;
    double  sample_rate;
    int     latency;
    /* Level matching: slow mean-square trackers either side of the shift,
       and the makeup gain they imply (ramped, never stepped). The input
       tracker is DELAY-ALIGNED to the output through a small ring of
       per-call energies: the output of process() is the input from
       ~latency samples ago, and comparing input-now against output-now
       pumps on anything percussive -- the attack reaches the input
       tracker a whole latency before it reaches the output one. */
    struct { int n; double msq; } ring[256];
    int     ring_head;
    long long fed; /* samples since create/reset: the ratio is meaningless
                      until at least a latency's worth has flowed through */
    double  ms_in;
    double  ms_out;
    double  makeup;
    std::atomic<float> makeup_mirror; /* for the status read-out */
};

int ae_shifter_block_samples (double sample_rate, int quality)
{
    (void) sample_rate;
    double ms;
    switch (quality)
    {
        case AE_SHIFT_QUALITY_LOW:  ms = 25.0;  break;
        case AE_SHIFT_QUALITY_HIGH: ms = 120.0; break;
        default:                    ms = 45.0;  break;
    }
    /* The block is a fixed SAMPLE count, referenced to the presets' 48 kHz
       millisecond names -- deliberately NOT scaled by the actual rate. At
       96 kHz the same samples span half the time, which is the entire
       latency win of running the interface fast: "balanced" becomes a
       22.5 ms block instead of 45. The cost is the analysis window
       shortening in time with it, so frequency resolution at the bottom of
       the range drops -- the low preset at 96 k is a 12.5 ms window, thin
       for a bass or a low guitar; step up a preset if the bottom warbles. */
    int block = (int) (48000.0 * ms / 1000.0);
    return block < 64 ? 64 : block;
}

AeShifter *ae_shifter_create (double sample_rate, int block_samples)
{
    if (sample_rate <= 0.0)
        sample_rate = 48000.0;
    if (block_samples <= 0)
        block_samples = ae_shifter_block_samples (sample_rate,
                                                  AE_SHIFT_QUALITY_BALANCED);

    AeShifter *s = new (std::nothrow) AeShifter();
    if (s == NULL)
        return NULL;
    s->sample_rate = sample_rate;
    /* interval = block/4 matches the library's own presetDefault ratio. */
    do_configure (s->stretch, block_samples, block_samples / 4,
                  std::integral_constant<bool, HasFormants<Stretch>::value>());
    s->latency = s->stretch.inputLatency() + s->stretch.outputLatency();
    std::memset (s->ring, 0, sizeof (s->ring));
    s->ring_head = 0;
    s->fed = 0;
    s->ms_in = s->ms_out = 0.0;
    s->makeup = 1.0;
    s->makeup_mirror.store (1.0f, std::memory_order_relaxed);
    return s;
}

void ae_shifter_destroy (AeShifter *s)
{
    delete s;
}

void ae_shifter_reset (AeShifter *s)
{
    if (s == NULL)
        return;
    s->stretch.reset();
    std::memset (s->ring, 0, sizeof (s->ring));
    s->ring_head = 0;
    s->fed = 0;
    s->ms_in = s->ms_out = 0.0;
    s->makeup = 1.0;
    s->makeup_mirror.store (1.0f, std::memory_order_relaxed);
}

float ae_shifter_makeup (const AeShifter *s)
{
    return s != NULL
        ? const_cast<AeShifter *> (s)->makeup_mirror.load (std::memory_order_relaxed)
        : 1.0f;
}

void ae_shifter_set_semitones (AeShifter *s, double semitones,
                               double tonality_limit_hz)
{
    if (s == NULL)
        return;
    /* The library takes the tonality limit normalised against the sample
       rate; 0 means a plain linear frequency map. */
    const float limit = tonality_limit_hz > 0.0
                          ? (float) (tonality_limit_hz / s->sample_rate) : 0.0f;
    s->stretch.setTransposeSemitones ((float) semitones, limit);
}

void ae_shifter_set_formant_semitones (AeShifter *s, double semitones,
                                       bool compensate)
{
    if (s == NULL)
        return;
    apply_formants (s->stretch, (float) semitones, compensate,
                    std::integral_constant<bool, HasFormants<Stretch>::value>());
}

void ae_shifter_set_formant_base (AeShifter *s, double base_hz)
{
    if (s == NULL)
        return;
    /* The library takes the base normalised against the sample rate. */
    apply_formant_base (s->stretch,
                        base_hz > 0.0 ? (float) (base_hz / s->sample_rate) : 0.0f,
                        std::integral_constant<bool, HasFormants<Stretch>::value>());
}

bool ae_shifter_has_formant_support (void)
{
    return HasFormants<Stretch>::value;
}

int ae_shifter_latency (const AeShifter *s)
{
    return s != NULL ? s->latency : 0;
}

void ae_shifter_process (AeShifter *s, const float *in, float *out, int n)
{
    if (s == NULL || n <= 0)
        return;
    /* The library never writes through the input pointer, but its buffer
       concept is non-const, so cast here rather than copying a block. */
    float *ip = const_cast<float *> (in);
    float **ipp = &ip;
    float **opp = &out;

    double sq_in = 0.0;
    for (int i = 0; i < n; ++i)
        sq_in += (double) in[i] * in[i];

    s->stretch.process (ipp, n, opp, n);

    /* Level matching. Transposition on its own is level-preserving, but
       FORMANT COMPENSATION is not: re-imposing the original spectral
       envelope on a shifted spectrum throws away real energy, and the
       further up you shift the more it costs -- measured here at roughly
       -4 dB at a fifth and -6 dB at an octave on a harmonic-rich source.
       That is a tonal choice with a level side effect, and the side effect
       is not wanted: a harmony voice asked for at 0 dB must arrive at the
       lead's level, not several dB under it with the operator's trim
       spending its range climbing back.

       So track the mean square either side of the shift over ~100 ms and
       restore what the envelope stage took. Slow on purpose: this must
       correct a standing spectral loss without following the note's own
       dynamics, which are the performance. The gain ramps across the block
       rather than stepping, and is clamped to +/-12 dB so a pathological
       block cannot turn into a burst. */
    double sq_out = 0.0;
    for (int i = 0; i < n; ++i)
        sq_out += (double) out[i] * out[i];

    /* Push this call's input energy and read back the entry ~latency
       samples old: THAT is the input this call's output was made from. */
    s->ring_head = (s->ring_head + 1) & 255;
    s->ring[s->ring_head].n   = n;
    s->ring[s->ring_head].msq = sq_in / n;
    double aligned_in = s->ring[s->ring_head].msq;
    {
        long long acc = 0;
        for (int k = 0; k < 256; ++k)
        {
            const int idx = (s->ring_head - k) & 255;
            if (s->ring[idx].n <= 0)
                break;
            aligned_in = s->ring[idx].msq;
            acc += s->ring[idx].n;
            if (acc >= (long long) s->latency)
                break;
        }
    }

    const double a = 1.0 - exp (-(double) n / (0.100 * s->sample_rate));
    s->ms_in  += (aligned_in  - s->ms_in)  * a;
    s->ms_out += (sq_out / n  - s->ms_out) * a;

    s->fed += n;
    double target;
    if (s->fed < (long long) s->latency + (long long) (0.15 * s->sample_rate))
        target = 1.0; /* output hasn't caught up with input yet: any ratio
                         measured across the gap is fiction */
    else if (s->ms_in <= 1e-8)
        target = s->makeup; /* silence: hold, don't divide noise by noise */
    else if (s->ms_out <= 0.0625 * s->ms_in)
        target = 1.0;       /* output missing (post-reset gap, dropout):
                               NEVER boost into a hole -- relax to unity */
    else
    {
        /* The formant stage costs at most ~6 dB at extreme shifts and
           well under 1 dB at correction ratios; +-6 dB of authority
           covers the real loss with no room for a runaway. */
        target = sqrt (s->ms_in / s->ms_out);
        if (target < 0.5) target = 0.5;
        if (target > 2.0) target = 2.0;
    }

    if (target != s->makeup || s->makeup != 1.0)
    {
        const double step = (target - s->makeup) / n;
        double g = s->makeup;
        for (int i = 0; i < n; ++i)
        {
            g += step;
            out[i] = (float) (out[i] * g);
        }
        s->makeup = target;
    }
    s->makeup_mirror.store ((float) s->makeup, std::memory_order_relaxed);
}

const char *ae_shifter_version (void)
{
    static char buf[32];
    std::snprintf (buf, sizeof (buf), "%d.%d.%d",
                   (int) Stretch::version[0], (int) Stretch::version[1],
                   (int) Stretch::version[2]);
    return buf;
}
