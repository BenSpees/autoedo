#include "shifter.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <cstdio>
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
    return s;
}

void ae_shifter_destroy (AeShifter *s)
{
    delete s;
}

void ae_shifter_reset (AeShifter *s)
{
    if (s != NULL)
        s->stretch.reset();
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
    s->stretch.process (ipp, n, opp, n);
}

const char *ae_shifter_version (void)
{
    static char buf[32];
    std::snprintf (buf, sizeof (buf), "%d.%d.%d",
                   (int) Stretch::version[0], (int) Stretch::version[1],
                   (int) Stretch::version[2]);
    return buf;
}
