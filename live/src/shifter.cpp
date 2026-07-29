#include "shifter.h"

#include "../third_party/signalsmith-stretch/signalsmith-stretch.h"

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
}

struct AeShifter
{
    Stretch stretch;
    double  sample_rate;
    int     latency;
};

int ae_shifter_block_samples (double sample_rate, int quality)
{
    if (sample_rate <= 0.0)
        sample_rate = 48000.0;
    double ms;
    switch (quality)
    {
        case AE_SHIFT_QUALITY_LOW:  ms = 25.0;  break;
        case AE_SHIFT_QUALITY_HIGH: ms = 120.0; break;
        default:                    ms = 45.0;  break;
    }
    int block = (int) (sample_rate * ms / 1000.0);
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
    s->stretch.configure (1, block_samples, block_samples / 4);
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
