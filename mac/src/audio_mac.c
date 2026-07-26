/* CoreAudio backend: two AUHAL units (one for the capture device, one for
   the playback device) bridged by a lock-free ring buffer, with a linear-
   interpolating resampler on the read side so the two devices may run at
   different nominal sample rates. All correction happens in the output
   unit's render callback. */

#include "audio.h"
#include "audio_params.h"
#include "psola.h"
#include "ring.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef kAudioObjectPropertyElementMain /* pre-macOS 12 SDKs */
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
    float *harm_l;     /* harmony-voice mix, left / right */
    float *harm_r;

    AePsola psola;

    double in_rate;
    double out_rate;
    int    out_channels;
    int    buffer_frames;
    int    latency_frames;

    char in_name[AE_NAME_MAX];
    char out_name[AE_NAME_MAX];

    /* Live parameters (any thread -> audio thread). */
    AeAtomicParams params;
};

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
    bool  bypass = false;
    float gain   = 1.0f;
    ae_atomic_params_apply (&e->params, &e->psola, &bypass, &gain);
    ae_psola_process (&e->psola, e->proc, e->harm_l, e->harm_r, (int) n);

    const float *src = bypass ? e->dry : e->proc;
    const bool   stereo = io->mNumberBuffers >= 2;

    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        float *dst = io->mBuffers[b].mData;
        const UInt32 cap = io->mBuffers[b].mDataByteSize / (UInt32) sizeof (float);
        const float *harm = ! stereo ? NULL : (b == 0 ? e->harm_l : e->harm_r);
        for (UInt32 i = 0; i < n_frames && i < cap; ++i)
        {
            if (i >= n) { dst[i] = 0.0f; continue; }
            float s = src[i];
            if (! bypass)
                s += harm != NULL ? harm[i]
                                  : 0.5f * (e->harm_l[i] + e->harm_r[i]); /* mono out */
            dst[i] = s * gain;
        }
    }
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
    ae_psola_free (&e->psola);
    free (e);
}

void ae_audio_engine_set_params (AeAudioEngine *e, const AeLiveParams *p)
{
    if (e != NULL)
        ae_atomic_params_store (&e->params, p);
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
    out->detected_hz     = ae_psola_detected_hz (&e->psola);
    out->target_hz       = ae_psola_target_hz (&e->psola);
    out->voiced          = ae_psola_voiced (&e->psola);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        out->harm_deg[v] = ae_psola_harm_degree (&e->psola, v);
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
    e->out_channels  = out_ch < 2 ? out_ch : 2;
    e->buffer_frames = cfg->buffer_frames >= 32 && cfg->buffer_frames <= 2048
                         ? cfg->buffer_frames : 256;

    set_device_buffer_size (in_dev,  e->buffer_frames);
    set_device_buffer_size (out_dev, e->buffer_frames);

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

    ae_psola_prepare (&e->psola, e->out_rate, MAX_FRAMES,
                      cfg->det_min_hz, cfg->det_max_hz);
    ae_audio_engine_set_params (e, &cfg->params);

    /* ~20 ms input-side cushion (or 3 hardware blocks, whichever is more)
       before playback starts, so device clock jitter can't underrun us. */
    uint64_t cushion = (uint64_t) (e->in_rate * 0.020);
    const uint64_t three_blocks = (uint64_t) (3.0 * e->buffer_frames * e->in_rate / e->out_rate);
    if (cushion < three_blocks)
        cushion = three_blocks;
    e->prefill_target = cushion;

    e->latency_frames = ae_psola_latency (&e->psola)
                      + (int) ((double) cushion * e->out_rate / e->in_rate)
                      + e->buffer_frames;

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

    return e;
}

void ae_audio_engine_stop (AeAudioEngine *e)
{
    if (e != NULL)
        engine_teardown (e);
}
