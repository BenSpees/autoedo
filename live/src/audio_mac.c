/* CoreAudio backend: two AUHAL units (one for the capture device, one for
   the playback device) bridged by a lock-free ring buffer, with a linear-
   interpolating resampler on the read side so the two devices may run at
   different nominal sample rates. All correction happens in the output
   unit's render callback. */

#include "audio.h"
#include "audio_params.h"
#include "corrector.h"
#include "ir_load.h"
#include "ring.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* kAudioObjectPropertyElementMain replaced ...Master in the macOS 12 SDK.
   Both are enum constants, not macros, so #ifndef on the name always fires
   and would alias us onto the deprecated one forever (six deprecation
   warnings per build). Gate on the SDK version instead. */
#include <AvailabilityMacros.h>
#if !defined(MAC_OS_VERSION_12_0) \
    || (defined(MAC_OS_X_VERSION_MAX_ALLOWED) \
        && MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_VERSION_12_0)
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

#define MAX_FRAMES 4096 /* per render callback */

/* ---------------------------------------------------------------- devices */

static bool cfstring_to_utf8 (CFStringRef s, char *out, size_t cap)
{
    if (s == NULL)
        return false;
    return CFStringGetCString (s, out, (CFIndex) cap, kCFStringEncodingUTF8);
}

static bool device_string_prop (AudioDeviceID dev, AudioObjectPropertySelector sel,
                                char *out, size_t cap)
{
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 size = sizeof (s);
    if (AudioObjectGetPropertyData (dev, &a, 0, NULL, &size, &s) != noErr || s == NULL)
        return false;
    const bool ok = cfstring_to_utf8 (s, out, cap);
    CFRelease (s);
    return ok;
}

static int device_channels (AudioDeviceID dev, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, scope,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (dev, &a, 0, NULL, &size) != noErr || size == 0)
        return 0;
    AudioBufferList *abl = malloc (size);
    if (abl == NULL)
        return 0;
    int ch = 0;
    if (AudioObjectGetPropertyData (dev, &a, 0, NULL, &size, abl) == noErr)
        for (UInt32 i = 0; i < abl->mNumberBuffers; ++i)
            ch += (int) abl->mBuffers[i].mNumberChannels;
    free (abl);
    return ch;
}

static double device_nominal_rate (AudioDeviceID dev)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    Float64 rate = 0.0;
    UInt32 size = sizeof (rate);
    if (AudioObjectGetPropertyData (dev, &a, 0, NULL, &size, &rate) != noErr)
        return 0.0;
    return rate;
}

static AudioDeviceID default_device (bool input)
{
    AudioObjectPropertyAddress a = { input ? kAudioHardwarePropertyDefaultInputDevice
                                           : kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32 size = sizeof (dev);
    AudioObjectGetPropertyData (kAudioObjectSystemObject, &a, 0, NULL, &size, &dev);
    return dev;
}

static int all_devices (AudioDeviceID **out, int *count)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr)
        return -1;
    AudioDeviceID *ids = malloc (size > 0 ? size : 1);
    if (ids == NULL)
        return -1;
    if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &a, 0, NULL, &size, ids) != noErr)
    {
        free (ids);
        return -1;
    }
    *out   = ids;
    *count = (int) (size / sizeof (AudioDeviceID));
    return 0;
}

int ae_audio_list_devices (AeDeviceInfo **out, int *count)
{
    AudioDeviceID *ids = NULL;
    int n = 0;
    if (all_devices (&ids, &n) != 0)
        return -1;

    AeDeviceInfo *infos = calloc ((size_t) (n > 0 ? n : 1), sizeof (AeDeviceInfo));
    if (infos == NULL)
    {
        free (ids);
        return -1;
    }

    const AudioDeviceID def_in  = default_device (true);
    const AudioDeviceID def_out = default_device (false);

    int m = 0;
    for (int i = 0; i < n; ++i)
    {
        AeDeviceInfo *d = &infos[m];
        if (! device_string_prop (ids[i], kAudioDevicePropertyDeviceUID, d->uid, sizeof (d->uid)))
            continue;
        if (! device_string_prop (ids[i], kAudioObjectPropertyName, d->name, sizeof (d->name)))
            snprintf (d->name, sizeof (d->name), "%s", d->uid);
        d->input_channels    = device_channels (ids[i], kAudioObjectPropertyScopeInput);
        d->output_channels   = device_channels (ids[i], kAudioObjectPropertyScopeOutput);
        d->nominal_rate      = device_nominal_rate (ids[i]);
        d->is_default_input  = (ids[i] == def_in);
        d->is_default_output = (ids[i] == def_out);
        if (d->input_channels == 0 && d->output_channels == 0)
            continue;
        ++m;
    }

    free (ids);
    *out   = infos;
    *count = m;
    return 0;
}

/* Resolve "" to the default device, otherwise match by UID. */
static AudioDeviceID find_device (const char *uid, bool input, char *err, size_t err_len)
{
    if (uid == NULL || uid[0] == '\0')
    {
        const AudioDeviceID dev = default_device (input);
        if (dev == kAudioObjectUnknown)
            snprintf (err, err_len, "no default %s device", input ? "input" : "output");
        return dev;
    }

    AudioDeviceID *ids = NULL;
    int n = 0;
    if (all_devices (&ids, &n) != 0)
    {
        snprintf (err, err_len, "device enumeration failed");
        return kAudioObjectUnknown;
    }
    AudioDeviceID found = kAudioObjectUnknown;
    for (int i = 0; i < n; ++i)
    {
        char u[AE_UID_MAX];
        if (device_string_prop (ids[i], kAudioDevicePropertyDeviceUID, u, sizeof (u))
            && strcmp (u, uid) == 0)
        {
            found = ids[i];
            break;
        }
    }
    free (ids);
    if (found == kAudioObjectUnknown)
        snprintf (err, err_len, "%s device not found: %s", input ? "input" : "output", uid);
    return found;
}

/* ----------------------------------------------------------------- engine */

struct AeAudioEngine
{
    AudioUnit in_unit;
    AudioUnit out_unit;
    bool      in_running, out_running;

    AeRing  ring;
    double  read_frac;      /* resampler fractional position (output cb only) */
    bool    prefilled;
    uint64_t prefill_target; /* input samples to buffer before playback starts */

    float *in_scratch; /* input callback capture buffer */
    float *proc;       /* mono processing block */
    float *dry;        /* latency-free dry copy for bypass */
    _Atomic float out_peak; /* decaying pre-clip peak of the summed output */
    int   send_client;  /* client channel index of the record send; -1 = none */
    float send_g_cur;   /* smoothed send gain (click-free mute) */
    float *harm_l;     /* harmony-voice mix, left / right */
    float *harm_r;

    AeCorrector corrector;

    double in_rate;
    double out_rate;
    int    out_channels;
    int    buffer_frames;
    AeTapRing tap;                /* local audio tap (see audio.h) */
    int    latency_frames;        /* the ENGINE's own path (what the
                                     record send is aligned by) */
    int    device_latency_frames; /* hardware overhead outside that path:
                                     input collection cycle + both sides'
                                     device/stream/safety latencies */

    char in_name[AE_NAME_MAX];
    char out_name[AE_NAME_MAX];

    /* MIDI input (CoreMIDI callback thread -> audio thread). */
    MIDIClientRef    midi_client;
    MIDIPortRef      midi_port;
    _Atomic uint64_t hw_midi_lo;
    _Atomic uint64_t hw_midi_hi;

    /* Live parameters (any thread -> audio thread). */
    AeAtomicParams params;
};

/* ------------------------------------------------------------------- MIDI */

static bool midi_source_name (MIDIEndpointRef src, char *out, size_t cap)
{
    CFStringRef s = NULL;
    if (MIDIObjectGetStringProperty (src, kMIDIPropertyDisplayName, &s) != noErr
        || s == NULL)
        return false;
    const bool ok = cfstring_to_utf8 (s, out, cap);
    CFRelease (s);
    return ok;
}

int ae_audio_list_midi_sources (char out[][AE_NAME_MAX], int max)
{
    const ItemCount n = MIDIGetNumberOfSources();
    int m = 0;
    for (ItemCount i = 0; i < n && m < max; ++i)
        if (midi_source_name (MIDIGetSource (i), out[m], AE_NAME_MAX))
            ++m;
    return m;
}

static void midi_note (AeAudioEngine *e, int note, bool on)
{
    if (note < 0 || note > 127)
        return;
    _Atomic uint64_t *word = note < 64 ? &e->hw_midi_lo : &e->hw_midi_hi;
    const uint64_t bit = 1ull << (note & 63);
    if (on)
        atomic_fetch_or_explicit (word, bit, memory_order_relaxed);
    else
        atomic_fetch_and_explicit (word, ~bit, memory_order_relaxed);
}

static void midi_read (const MIDIPacketList *pktlist, void *ref_con, void *src_ref)
{
    (void) src_ref;
    AeAudioEngine *e = ref_con;
    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; ++i)
    {
        const Byte *d = pkt->data;
        UInt16 len = pkt->length;
        UInt16 k = 0;
        Byte status = 0;
        while (k < len)
        {
            if (d[k] & 0x80)
                status = d[k++];
            if (status == 0)
            {
                ++k;
                continue;
            }
            const Byte hi = status & 0xF0;
            if (hi == 0x90 && k + 1 < len)      /* note on (vel 0 = off) */
            {
                midi_note (e, d[k], d[k + 1] > 0);
                k += 2;
            }
            else if (hi == 0x80 && k + 1 < len) /* note off */
            {
                midi_note (e, d[k], false);
                k += 2;
            }
            else if (hi == 0xC0 || hi == 0xD0)  /* 1-data-byte messages */
                k += 1;
            else if (hi >= 0x80 && hi <= 0xE0)  /* other 2-data-byte messages */
                k += 2;
            else                                 /* system: skip byte-wise */
                ++k;
        }
        pkt = MIDIPacketNext (pkt);
    }
}

/* Connect MIDI input. Non-fatal: audio runs fine without it. */
static void midi_setup (AeAudioEngine *e, const char *source_name)
{
    if (MIDIClientCreate (CFSTR ("AutoEDO"), NULL, NULL, &e->midi_client) != noErr)
        return;
    if (MIDIInputPortCreate (e->midi_client, CFSTR ("AutoEDO In"), midi_read,
                             e, &e->midi_port) != noErr)
    {
        MIDIClientDispose (e->midi_client);
        e->midi_client = 0;
        return;
    }
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; ++i)
    {
        MIDIEndpointRef src = MIDIGetSource (i);
        if (source_name != NULL && source_name[0] != '\0')
        {
            char name[AE_NAME_MAX];
            if (! midi_source_name (src, name, sizeof (name))
                || strcmp (name, source_name) != 0)
                continue; /* a named source: connect only that one */
        }
        MIDIPortConnectSource (e->midi_port, src, NULL);
    }
}

static OSStatus input_cb (void *ref, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus, UInt32 n_frames,
                          AudioBufferList *unused)
{
    (void) unused;
    AeAudioEngine *e = ref;
    if (n_frames > MAX_FRAMES)
        n_frames = MAX_FRAMES;

    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 1;
    abl.mBuffers[0].mDataByteSize   = n_frames * (UInt32) sizeof (float);
    abl.mBuffers[0].mData           = e->in_scratch;

    const OSStatus st = AudioUnitRender (e->in_unit, flags, ts, bus, n_frames, &abl);
    if (st != noErr)
        return st;

    ae_ring_write (&e->ring, e->in_scratch, n_frames);
    return noErr;
}

static OSStatus render_cb (void *ref, AudioUnitRenderActionFlags *flags,
                           const AudioTimeStamp *ts, UInt32 bus, UInt32 n_frames,
                           AudioBufferList *io)
{
    (void) flags; (void) ts; (void) bus;
    AeAudioEngine *e = ref;
    UInt32 n = n_frames;
    if (n > MAX_FRAMES)
        n = MAX_FRAMES;

    /* Base resample ratio, plus a gentle nudge that keeps the ring near its
       target fill so independent device clocks can't drift it empty/full. */
    const uint64_t fill = ae_ring_fill (&e->ring);
    double ratio = e->in_rate / e->out_rate;

    int got = 0;
    if (! e->prefilled)
    {
        if (fill >= e->prefill_target)
            e->prefilled = true;
    }
    if (e->prefilled)
    {
        double adj = 1.0 + 0.05 * ((double) fill - (double) e->prefill_target)
                                / (double) e->prefill_target;
        if (adj < 0.98) adj = 0.98;
        if (adj > 1.02) adj = 1.02;
        got = ae_ring_read_lerp (&e->ring, e->proc, (int) n, ratio * adj, &e->read_frac);
        if (got < (int) n)
            e->prefilled = false; /* underrun: rebuild the cushion */
    }
    for (int i = got; i < (int) n; ++i)
        e->proc[i] = 0.0f;

    memcpy (e->dry, e->proc, n * sizeof (float));

    /* Apply live parameters, then correct. */
    AeMixParams mix;
    ae_atomic_params_apply (&e->params, &e->corrector,
                            atomic_load_explicit (&e->hw_midi_lo, memory_order_relaxed),
                            atomic_load_explicit (&e->hw_midi_hi, memory_order_relaxed),
                            &mix);
    const bool  bypass = mix.bypass;
    const bool  lead   = mix.lead_on;
    const float lead_g = mix.lead_gain;
    const float gain   = mix.master_gain;
    ae_corrector_process (&e->corrector, e->proc, e->harm_l, e->harm_r, (int) n);

    const float *src = bypass ? e->dry : e->proc;
    /* bypassOutput: what bypass PUTS on the output. "dry" passes the input
       through (byp_g 1), "mute" puts silence there (byp_g 0) for a rig
       whose dry already reaches the desk on its own row. Scaling the dry
       rather than branching keeps bypass a single decision. */
    const float  byp_g = ae_bypass_gain (&mix);
    const bool   stereo = (int) io->mNumberBuffers - (e->send_client >= 0 ? 1 : 0) >= 2;

    float pk = atomic_load_explicit (&e->out_peak, memory_order_relaxed) * 0.98f;

    /* The record send: its own client channel, its own content and trim,
       never the PA's. Gain is smoothed so sendOn is a click-free mute. */
    const float *wet = ae_corrector_lead_wet (&e->corrector);
    const float  send_target = mix.send_on ? mix.send_gain : 0.0f;

    if (atomic_load_explicit (&e->tap.on, memory_order_relaxed))
    {
        const int content = atomic_load_explicit (&e->tap.content,
                                                  memory_order_relaxed);
        for (UInt32 i = 0; i < n_frames && i < n; ++i)
        {
            const float hm   = 0.5f * (e->harm_l[i] + e->harm_r[i]);
            const float full = (bypass ? byp_g * e->dry[i]
                                       : (lead ? lead_g * src[i] : 0.0f) + hm)
                             * gain;
            ae_tap_push (&e->tap, ae_tap_value (content,
                                                wet ? wet[i] : 0.0f, hm, full));
        }
    }

    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        float *dst = io->mBuffers[b].mData;
        const UInt32 cap = io->mBuffers[b].mDataByteSize / (UInt32) sizeof (float);
        if (e->send_client >= 0 && b == (UInt32) e->send_client)
        {
            float sg = e->send_g_cur;
            for (UInt32 i = 0; i < n_frames && i < cap; ++i)
            {
                if (i >= n) { dst[i] = 0.0f; continue; }
                sg += (send_target - sg) * 0.0005f;
                const float hm = 0.5f * (e->harm_l[i] + e->harm_r[i]);
                float sv;
                switch (mix.send_content)
                {
                    case AE_SEND_WET:  sv = (wet ? wet[i] : 0.0f) + hm; break;
                    case AE_SEND_LEAD: sv = wet ? wet[i] : 0.0f;       break;
                    case AE_SEND_HARM: sv = hm;                        break;
                    default: /* FULL: exactly the live mono mix */
                        sv = (bypass ? byp_g * e->dry[i]
                                     : (lead ? lead_g * src[i] : 0.0f) + hm)
                           * gain;
                        break;
                }
                dst[i] = ae_soft_clip (sv * sg);
            }
            e->send_g_cur = sg;
            continue;
        }
        const float *harm = ! stereo ? NULL : (b == 0 ? e->harm_l : e->harm_r);
        for (UInt32 i = 0; i < n_frames && i < cap; ++i)
        {
            if (i >= n) { dst[i] = 0.0f; continue; }
            /* Lead mute is harmony-only output; bypass still wins (a dry
               passthrough with the lead "muted" would be silence). */
            float s = bypass ? byp_g * src[i] : (lead ? lead_g * src[i] : 0.0f);
            if (! bypass)
                s += harm != NULL ? harm[i]
                                  : 0.5f * (e->harm_l[i] + e->harm_r[i]); /* mono out */
            const float pre = fabsf (s * gain);
            if (pre > pk) pk = pre;
            dst[i] = ae_soft_clip (s * gain);
        }
    }
    atomic_store_explicit (&e->out_peak, pk, memory_order_relaxed);
    return noErr;
}

static void set_device_buffer_size (AudioDeviceID dev, int frames)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 v = (UInt32) frames;
    AudioObjectSetPropertyData (dev, &a, 0, NULL, sizeof (v), &v); /* best-effort */
}

/* What the hardware actually GRANTED -- the set above is best-effort,
   and a device already open elsewhere keeps its owner's size. Latency
   arithmetic must follow what the device does, not what we asked for:
   using the requested size when the grant differed understated the
   stated latency silently. */
static int get_device_buffer_size (AudioDeviceID dev, int fallback)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 v = 0, sz = sizeof (v);
    if (AudioObjectGetPropertyData (dev, &a, 0, NULL, &sz, &v) == noErr && v > 0)
        return (int) v;
    return fallback;
}

/* One side's hardware latency, in that device's own sample frames:
   device latency + safety offset + the first stream's latency -- the
   converters and USB transport live in these numbers, and CoreAudio
   only tells you if you ask. Best-effort: a property the device does
   not publish counts as zero, so the figure can only be honest-or-low,
   never invented. */
static int device_side_latency (AudioDeviceID dev, bool is_input)
{
    const AudioObjectPropertyScope scope =
        is_input ? kAudioObjectPropertyScopeInput
                 : kAudioObjectPropertyScopeOutput;
    int total = 0;
    UInt32 v = 0, sz = sizeof (v);
    AudioObjectPropertyAddress a = { kAudioDevicePropertyLatency, scope,
                                     kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData (dev, &a, 0, NULL, &sz, &v) == noErr)
        total += (int) v;
    a.mSelector = kAudioDevicePropertySafetyOffset;
    v = 0; sz = sizeof (v);
    if (AudioObjectGetPropertyData (dev, &a, 0, NULL, &sz, &v) == noErr)
        total += (int) v;
    a.mSelector = kAudioDevicePropertyStreams;
    AudioStreamID streams[8];
    sz = sizeof (streams);
    if (AudioObjectGetPropertyData (dev, &a, 0, NULL, &sz, streams) == noErr
        && sz >= sizeof (AudioStreamID))
    {
        AudioObjectPropertyAddress sa = { kAudioStreamPropertyLatency,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        v = 0; sz = sizeof (v);
        if (AudioObjectGetPropertyData (streams[0], &sa, 0, NULL, &sz, &v)
                == noErr)
            total += (int) v;
    }
    return total;
}

static OSStatus make_hal_unit (AudioUnit *out_unit)
{
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                       kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext (NULL, &desc);
    if (comp == NULL)
        return -1;
    return AudioComponentInstanceNew (comp, out_unit);
}

static AudioStreamBasicDescription float_format (double rate, int channels)
{
    AudioStreamBasicDescription f;
    memset (&f, 0, sizeof (f));
    f.mSampleRate       = rate;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mFramesPerPacket  = 1;
    f.mChannelsPerFrame = (UInt32) channels;
    f.mBitsPerChannel   = 32;
    f.mBytesPerFrame    = sizeof (float); /* per channel when non-interleaved */
    f.mBytesPerPacket   = sizeof (float);
    return f;
}

static void engine_teardown (AeAudioEngine *e)
{
    if (e->midi_client != 0)
        MIDIClientDispose (e->midi_client); /* disposes the port too */
    if (e->in_unit != NULL)
    {
        if (e->in_running)
            AudioOutputUnitStop (e->in_unit);
        AudioUnitUninitialize (e->in_unit);
        AudioComponentInstanceDispose (e->in_unit);
    }
    if (e->out_unit != NULL)
    {
        if (e->out_running)
            AudioOutputUnitStop (e->out_unit);
        AudioUnitUninitialize (e->out_unit);
        AudioComponentInstanceDispose (e->out_unit);
    }
    ae_ring_free (&e->ring);
    free (e->in_scratch);
    free (e->proc);
    free (e->dry);
    free (e->harm_l);
    free (e->harm_r);
    ae_corrector_free (&e->corrector);
    free (e);
}

void ae_audio_engine_set_tap (AeAudioEngine *e, bool on, int content)
{
    if (e == NULL)
        return;
    atomic_store_explicit (&e->tap.content, content, memory_order_relaxed);
    atomic_store_explicit (&e->tap.on, on, memory_order_relaxed);
}

int ae_audio_engine_tap_read (AeAudioEngine *e, float *out, int max_samples,
                              long long *first_sample)
{
    if (e == NULL)
        return 0;
    return ae_tap_ring_read (&e->tap, out, max_samples, first_sample);
}

void ae_audio_engine_set_params (AeAudioEngine *e, const AeLiveParams *p)
{
    if (e != NULL)
        ae_atomic_params_store (&e->params, p);
    atomic_store_explicit (&e->params.follow_level, 1.0, memory_order_relaxed);
}


bool ae_audio_engine_load_samples (AeAudioEngine *e, const char *root,
                                   const char *instrument, const char *manifest,
                                   int octave, char *err, size_t err_len)
{
    if (e == NULL)
    {
        snprintf (err, err_len, "engine not running");
        return false;
    }
    ae_corrector_set_sample_octave (&e->corrector, octave);
    const bool ok = ae_corrector_load_samples (&e->corrector, root, instrument,
                                               manifest, err, err_len);
    /* Hold off long enough for the audio thread to turn a block over before
       returning: the slot this load retired must be safely dead before the
       NEXT load is allowed to refill it. */
    ae_engine_sleep_ms (60);
    return ok;
}

bool ae_audio_engine_load_ir (AeAudioEngine *e, int point, const char *path,
                              const char *hash, double predelay_ms,
                              char *err, size_t err_len)
{
    if (e == NULL)
    {
        snprintf (err, err_len, "engine not running");
        return false;
    }
    return ae_ir_load_point (&e->corrector, point, path, hash, predelay_ms,
                             e->corrector.fs, err, err_len);
}

void ae_audio_engine_set_follow (AeAudioEngine *e, int note, double level)
{
    atomic_store_explicit (&e->params.follow_note_p1,
                           note >= 0 && note < 128 ? note + 1 : 0,
                           memory_order_relaxed);
    atomic_store_explicit (&e->params.follow_level, level, memory_order_relaxed);
}

double ae_audio_engine_env (AeAudioEngine *e)
{
    return (double) ae_corrector_env (&e->corrector);
}

double ae_audio_engine_follow_level (AeAudioEngine *e)
{
    return atomic_load_explicit (&e->params.follow_level, memory_order_relaxed);
}

int ae_audio_engine_lead_degree (AeAudioEngine *e)
{
    return ae_corrector_lead_degree (&e->corrector);
}

void ae_audio_engine_set_midi_notes (AeAudioEngine *e, uint64_t lo, uint64_t hi)
{
    if (e == NULL)
        return;
    atomic_store_explicit (&e->params.vmidi_lo, lo, memory_order_relaxed);
    atomic_store_explicit (&e->params.vmidi_hi, hi, memory_order_relaxed);
}

void ae_audio_engine_get_status (AeAudioEngine *e, AeEngineStatus *out)
{
    memset (out, 0, sizeof (*out));
    if (e == NULL)
        return;
    out->running         = true;
    out->input_rate      = e->in_rate;
    out->output_rate     = e->out_rate;
    out->latency_samples = e->latency_frames;
    out->device_latency_samples = e->device_latency_frames;
    out->detected_hz     = ae_corrector_detected_hz (&e->corrector);
    out->target_hz       = ae_corrector_target_hz (&e->corrector);
    out->shift_st        = ae_corrector_shift_st (&e->corrector);
    out->shift_st_min    = ae_corrector_shift_st_min (&e->corrector);
    out->shift_st_max    = ae_corrector_shift_st_max (&e->corrector);
    out->lead_makeup     = ae_corrector_lead_makeup (&e->corrector);
    out->sample_vel      = ae_corrector_sample_vel (&e->corrector);
    out->sample_vel_ref  = ae_corrector_sample_vel_ref (&e->corrector);
    out->poly_notes_live = ae_corrector_poly_active (&e->corrector);
    for (int k = 0; k < AE_POLY_STATUS_MAX; ++k)
        out->poly_note[k] = ae_corrector_poly_note (&e->corrector, k);
    {
        const int lv = atomic_load_explicit (&e->corrector.smp_live, memory_order_relaxed);
        out->sample_zones = lv >= 0 ? e->corrector.smp_bank[lv].n_zones : 0;
        out->sample_files = lv >= 0 ? e->corrector.smp_bank[lv].n_recs  : 0;
        out->sample_norm_db = lv >= 0 && e->corrector.smp_bank[lv].norm > 0.0
            ? (float) (20.0 * log10 (e->corrector.smp_bank[lv].norm)) : 0.0f;
        out->sample_octave  = lv >= 0 ? e->corrector.smp_bank[lv].octave : 0;
        out->sample_clipped = lv >= 0 ? e->corrector.smp_bank[lv].clipped : 0;
    }
    out->out_peak        = atomic_load_explicit (&e->out_peak, memory_order_relaxed);
    out->voiced          = ae_corrector_voiced (&e->corrector);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        out->harm_deg[v] = ae_corrector_harm_degree (&e->corrector, v);
    out->trace_len = ae_corrector_trace (&e->corrector, &out->trace_seq,
                                         out->trace_det, out->trace_tgt, AE_TRACE_MAX);
    {
        uint64_t flo, fhi;
        ae_follow_bits (&e->params, &flo, &fhi);
        out->midi_held_lo = flo
                      | atomic_load_explicit (&e->hw_midi_lo, memory_order_relaxed)
                      | atomic_load_explicit (&e->params.vmidi_lo, memory_order_relaxed);
        out->midi_held_hi = fhi
                      | atomic_load_explicit (&e->hw_midi_hi, memory_order_relaxed)
                      | atomic_load_explicit (&e->params.vmidi_hi, memory_order_relaxed);
    }
    snprintf (out->input_name,  sizeof (out->input_name),  "%s", e->in_name);
    snprintf (out->output_name, sizeof (out->output_name), "%s", e->out_name);
}

AeAudioEngine *ae_audio_engine_start (const AeEngineConfig *cfg, char *err, size_t err_len)
{
    err[0] = '\0';

    AeAudioEngine *e = calloc (1, sizeof (*e));
    if (e == NULL)
    {
        snprintf (err, err_len, "out of memory");
        return NULL;
    }

    const AudioDeviceID in_dev = find_device (cfg->input_uid, true, err, err_len);
    if (in_dev == kAudioObjectUnknown)
    {
        free (e);
        return NULL;
    }
    const AudioDeviceID out_dev = find_device (cfg->output_uid, false, err, err_len);
    if (out_dev == kAudioObjectUnknown)
    {
        free (e);
        return NULL;
    }

    if (device_channels (in_dev, kAudioObjectPropertyScopeInput) < 1)
    {
        char name[AE_NAME_MAX] = "?";
        device_string_prop (in_dev, kAudioObjectPropertyName, name, sizeof (name));
        snprintf (err, err_len, "device \"%s\" has no input channels", name);
        free (e);
        return NULL;
    }
    const int out_ch = device_channels (out_dev, kAudioObjectPropertyScopeOutput);
    if (out_ch < 1)
    {
        char name[AE_NAME_MAX] = "?";
        device_string_prop (out_dev, kAudioObjectPropertyName, name, sizeof (name));
        snprintf (err, err_len, "device \"%s\" has no output channels", name);
        free (e);
        return NULL;
    }

    device_string_prop (in_dev,  kAudioObjectPropertyName, e->in_name,  sizeof (e->in_name));
    device_string_prop (out_dev, kAudioObjectPropertyName, e->out_name, sizeof (e->out_name));

    e->in_rate  = device_nominal_rate (in_dev);
    e->out_rate = device_nominal_rate (out_dev);
    if (e->in_rate <= 0.0)  e->in_rate  = 44100.0;
    if (e->out_rate <= 0.0) e->out_rate = 44100.0;
    /* A bound output channel renders mono (voice + harmony folded) onto that
       one device channel; the default is stereo on the first two. */
    if (cfg->output_channel > 0 && cfg->output_channel > out_ch)
    {
        snprintf (err, err_len, "device \"%s\" has no output channel %d (%d available)",
                  e->out_name, cfg->output_channel, out_ch);
        free (e);
        return NULL;
    }
    if (cfg->send_channel > 0 && cfg->send_channel > out_ch)
    {
        snprintf (err, err_len, "device \"%s\" has no output channel %d for the record send (%d available)",
                  e->out_name, cfg->send_channel, out_ch);
        free (e);
        return NULL;
    }
    e->out_channels  = cfg->output_channel > 0 ? 1 : (out_ch < 2 ? out_ch : 2);
    e->send_client   = cfg->send_channel > 0 ? e->out_channels : -1;
    if (e->send_client >= 0)
        e->out_channels += 1; /* one extra client channel rides to the map */
    e->send_g_cur    = 0.0f;
    e->buffer_frames = cfg->buffer_frames >= 32 && cfg->buffer_frames <= 2048
                         ? cfg->buffer_frames : 256;

    set_device_buffer_size (in_dev,  e->buffer_frames);
    set_device_buffer_size (out_dev, e->buffer_frames);
    /* every latency figure below follows the GRANTED sizes */
    const int buf_in  = get_device_buffer_size (in_dev,  e->buffer_frames);
    const int buf_out = get_device_buffer_size (out_dev, e->buffer_frames);

    /* Buffers. */
    if (ae_ring_init (&e->ring, 1 << 16) != 0)
    {
        snprintf (err, err_len, "out of memory");
        engine_teardown (e);
        return NULL;
    }
    e->in_scratch = calloc (MAX_FRAMES, sizeof (float));
    e->proc       = calloc (MAX_FRAMES, sizeof (float));
    e->dry        = calloc (MAX_FRAMES, sizeof (float));
    e->harm_l     = calloc (MAX_FRAMES, sizeof (float));
    e->harm_r     = calloc (MAX_FRAMES, sizeof (float));
    if (e->in_scratch == NULL || e->proc == NULL || e->dry == NULL
        || e->harm_l == NULL || e->harm_r == NULL)
    {
        snprintf (err, err_len, "out of memory");
        engine_teardown (e);
        return NULL;
    }

    ae_corrector_prepare (&e->corrector, e->out_rate, MAX_FRAMES,
                          cfg->det_min_hz, cfg->det_max_hz,
                          cfg->quality | (cfg->poly_mode ? AE_SHIFT_QUALITY_POLY_FLAG : 0));
    ae_audio_engine_set_params (e, &cfg->params);

    /* ~20 ms input-side cushion (or 3 hardware blocks, whichever is more)
       before playback starts, so device clock jitter can't underrun us.
       The block floor uses the GRANTED input buffer -- it is already in
       input-rate frames, and it is the cycle the jitter actually rides. */
    uint64_t cushion = (uint64_t) (e->in_rate * 0.020);
    const uint64_t three_blocks = (uint64_t) (3 * buf_in);
    if (cushion < three_blocks)
        cushion = three_blocks;
    e->prefill_target = cushion;

    e->latency_frames = ae_corrector_latency (&e->corrector)
                      + (int) ((double) cushion * e->out_rate / e->in_rate)
                      + buf_out;

    /* Hardware overhead OUTSIDE the engine's path, reported separately
       (deviceLatencyMs): the input side's collection cycle -- capture
       arrives one input buffer late, a cost the formula above never
       carried -- plus both sides' device/stream/safety latencies.
       processLatencyMs keeps meaning the engine's own path, which is
       what the record send is aligned by; the panel's honest
       mic-to-speaker figure is the SUM (totalLatencyMs). */
    e->device_latency_frames =
        (int) ((double) (buf_in + device_side_latency (in_dev, true))
               * e->out_rate / e->in_rate)
        + device_side_latency (out_dev, false);

    OSStatus st;
    const UInt32 on = 1, off = 0, max_slice = MAX_FRAMES;

    /* --- Input AUHAL: capture only, mono float client format. ------------- */
    if ((st = make_hal_unit (&e->in_unit)) != noErr)
    {
        snprintf (err, err_len, "input AUHAL create failed (%d)", (int) st);
        engine_teardown (e);
        return NULL;
    }
    AudioUnitSetProperty (e->in_unit, kAudioOutputUnitProperty_EnableIO,
                          kAudioUnitScope_Input, 1, &on, sizeof (on));
    AudioUnitSetProperty (e->in_unit, kAudioOutputUnitProperty_EnableIO,
                          kAudioUnitScope_Output, 0, &off, sizeof (off));
    if ((st = AudioUnitSetProperty (e->in_unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, 0, &in_dev, sizeof (in_dev))) != noErr)
    {
        snprintf (err, err_len, "cannot open input device \"%s\" (%d)", e->in_name, (int) st);
        engine_teardown (e);
        return NULL;
    }

    AudioStreamBasicDescription in_fmt = float_format (e->in_rate, 1);
    if ((st = AudioUnitSetProperty (e->in_unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Output, 1, &in_fmt, sizeof (in_fmt))) != noErr)
    {
        snprintf (err, err_len, "input format not accepted (%d)", (int) st);
        engine_teardown (e);
        return NULL;
    }

    /* Bind a specific device capture channel to the mono client stream.
       Without a map AUHAL folds in the device's first channel — fine for a
       microphone, wrong for a multi-input interface where each instance of
       the engine owns one input (e.g. 1 = voice, 2 = guitar). */
    if (cfg->input_channel > 0)
    {
        const int in_ch = device_channels (in_dev, kAudioObjectPropertyScopeInput);
        if (cfg->input_channel > in_ch)
        {
            snprintf (err, err_len, "device \"%s\" has no input channel %d (%d available)",
                      e->in_name, cfg->input_channel, in_ch);
            engine_teardown (e);
            return NULL;
        }
        const SInt32 map[1] = { (SInt32) (cfg->input_channel - 1) };
        if ((st = AudioUnitSetProperty (e->in_unit, kAudioOutputUnitProperty_ChannelMap,
                                        kAudioUnitScope_Output, 1, map, sizeof (map))) != noErr)
        {
            snprintf (err, err_len, "input channel %d not accepted (%d)",
                      cfg->input_channel, (int) st);
            engine_teardown (e);
            return NULL;
        }
    }

    AURenderCallbackStruct in_cb = { input_cb, e };
    AudioUnitSetProperty (e->in_unit, kAudioOutputUnitProperty_SetInputCallback,
                          kAudioUnitScope_Global, 0, &in_cb, sizeof (in_cb));
    AudioUnitSetProperty (e->in_unit, kAudioUnitProperty_MaximumFramesPerSlice,
                          kAudioUnitScope_Global, 0, &max_slice, sizeof (max_slice));

    if ((st = AudioUnitInitialize (e->in_unit)) != noErr)
    {
        snprintf (err, err_len, "input unit init failed (%d) — check microphone "
                                "permission in System Settings > Privacy", (int) st);
        engine_teardown (e);
        return NULL;
    }

    /* --- Output AUHAL: playback only. ------------------------------------- */
    if ((st = make_hal_unit (&e->out_unit)) != noErr)
    {
        snprintf (err, err_len, "output AUHAL create failed (%d)", (int) st);
        engine_teardown (e);
        return NULL;
    }
    AudioUnitSetProperty (e->out_unit, kAudioOutputUnitProperty_EnableIO,
                          kAudioUnitScope_Output, 0, &on, sizeof (on));
    AudioUnitSetProperty (e->out_unit, kAudioOutputUnitProperty_EnableIO,
                          kAudioUnitScope_Input, 1, &off, sizeof (off));
    if ((st = AudioUnitSetProperty (e->out_unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, 0, &out_dev, sizeof (out_dev))) != noErr)
    {
        snprintf (err, err_len, "cannot open output device \"%s\" (%d)", e->out_name, (int) st);
        engine_teardown (e);
        return NULL;
    }

    AudioStreamBasicDescription out_fmt = float_format (e->out_rate, e->out_channels);
    if ((st = AudioUnitSetProperty (e->out_unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Input, 0, &out_fmt, sizeof (out_fmt))) != noErr)
    {
        snprintf (err, err_len, "output format not accepted (%d)", (int) st);
        engine_teardown (e);
        return NULL;
    }

    /* Route the mono client onto the one requested device channel. The output
       map has one entry PER DEVICE CHANNEL, each naming the client channel
       that feeds it (-1 = silence) -- the mirror of the input map's shape. */
    if (cfg->output_channel > 0 || cfg->send_channel > 0)
    {
        SInt32 map[64];
        int n_map = out_ch;
        if (n_map > 64)
            n_map = 64;
        for (int i = 0; i < n_map; ++i)
            map[i] = -1;
        if (cfg->output_channel > 0)
            map[cfg->output_channel - 1] = 0;      /* mono live path */
        else
        {
            if (n_map > 0) map[0] = 0;             /* default stereo */
            if (n_map > 1) map[1] = 1;
        }
        if (e->send_client >= 0 && cfg->send_channel <= n_map)
            map[cfg->send_channel - 1] = e->send_client;
        if ((st = AudioUnitSetProperty (e->out_unit, kAudioOutputUnitProperty_ChannelMap,
                                        kAudioUnitScope_Output, 0, map,
                                        (UInt32) (n_map * (int) sizeof (SInt32)))) != noErr)
        {
            snprintf (err, err_len, "output channel map not accepted (%d)", (int) st);
            engine_teardown (e);
            return NULL;
        }
    }

    AURenderCallbackStruct out_cb = { render_cb, e };
    AudioUnitSetProperty (e->out_unit, kAudioUnitProperty_SetRenderCallback,
                          kAudioUnitScope_Input, 0, &out_cb, sizeof (out_cb));
    AudioUnitSetProperty (e->out_unit, kAudioUnitProperty_MaximumFramesPerSlice,
                          kAudioUnitScope_Global, 0, &max_slice, sizeof (max_slice));

    if ((st = AudioUnitInitialize (e->out_unit)) != noErr)
    {
        snprintf (err, err_len, "output unit init failed (%d)", (int) st);
        engine_teardown (e);
        return NULL;
    }

    /* --- Go. --------------------------------------------------------------- */
    if ((st = AudioOutputUnitStart (e->in_unit)) != noErr)
    {
        snprintf (err, err_len, "input start failed (%d) — check microphone "
                                "permission in System Settings > Privacy", (int) st);
        engine_teardown (e);
        return NULL;
    }
    e->in_running = true;

    if ((st = AudioOutputUnitStart (e->out_unit)) != noErr)
    {
        snprintf (err, err_len, "output start failed (%d)", (int) st);
        engine_teardown (e);
        return NULL;
    }
    e->out_running = true;

    midi_setup (e, cfg->midi_source); /* non-fatal */

    return e;
}

void ae_audio_engine_stop (AeAudioEngine *e)
{
    if (e != NULL)
        engine_teardown (e);
}

bool ae_audio_backend_embedded (void) { return false; }
