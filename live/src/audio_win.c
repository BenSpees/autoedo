/* Windows 10 backend: WASAPI shared-mode capture + render clients (which may
   be different devices at different mix rates) bridged by the same lock-free
   ring buffer / resampling reader as the CoreAudio backend, with winmm MIDI
   input for MIDI Harmony. Event-driven worker threads; all correction runs
   on the render thread. Requires only ole32 + winmm + the mingw-w64
   toolchain (C11 atomics + winpthreads). */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "audio_params.h"
#include "corrector.h"
#include "ir_load.h"
#include "ring.h"

#define MAX_FRAMES 4096

/* GUIDs defined locally so no uuid import library is needed. */
static const CLSID k_CLSID_MMDeviceEnumerator =
    {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const IID k_IID_IMMDeviceEnumerator =
    {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const IID k_IID_IAudioClient =
    {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const IID k_IID_IAudioCaptureClient =
    {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static const IID k_IID_IAudioRenderClient =
    {0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};
static const PROPERTYKEY k_PKEY_Device_FriendlyName =
    {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}}, 14};
static const GUID k_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID k_KSDATAFORMAT_SUBTYPE_PCM =
    {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

static void wide_to_utf8 (const WCHAR *w, char *out, int cap)
{
    out[0] = '\0';
    if (w != NULL)
        WideCharToMultiByte (CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    out[cap - 1] = '\0';
}

/* Sample layout of a shared-mode mix format. */
typedef struct
{
    int  channels;
    int  rate;
    bool is_float; /* else 16-bit PCM */
} WinFormat;

static bool parse_format (const WAVEFORMATEX *fx, WinFormat *out)
{
    out->channels = fx->nChannels;
    out->rate     = (int) fx->nSamplesPerSec;
    if (fx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fx->wBitsPerSample == 32)
    {
        out->is_float = true;
        return true;
    }
    if (fx->wFormatTag == WAVE_FORMAT_PCM && fx->wBitsPerSample == 16)
    {
        out->is_float = false;
        return true;
    }
    if (fx->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && fx->cbSize >= sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX))
    {
        const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *) fx;
        if (memcmp (&ex->SubFormat, &k_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
                    sizeof (GUID)) == 0 && fx->wBitsPerSample == 32)
        {
            out->is_float = true;
            return true;
        }
        if (memcmp (&ex->SubFormat, &k_KSDATAFORMAT_SUBTYPE_PCM,
                    sizeof (GUID)) == 0 && fx->wBitsPerSample == 16)
        {
            out->is_float = false;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- devices */

static IMMDeviceEnumerator *make_enumerator (void)
{
    IMMDeviceEnumerator *enu = NULL;
    if (FAILED (CoCreateInstance (&k_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &k_IID_IMMDeviceEnumerator, (void **) &enu)))
        return NULL;
    return enu;
}

static bool device_uid (IMMDevice *dev, char *out, int cap)
{
    LPWSTR id = NULL;
    if (FAILED (IMMDevice_GetId (dev, &id)) || id == NULL)
        return false;
    wide_to_utf8 (id, out, cap);
    CoTaskMemFree (id);
    return true;
}

static void device_name (IMMDevice *dev, char *out, int cap)
{
    out[0] = '\0';
    IPropertyStore *props = NULL;
    if (SUCCEEDED (IMMDevice_OpenPropertyStore (dev, STGM_READ, &props)))
    {
        PROPVARIANT v;
        PropVariantInit (&v);
        if (SUCCEEDED (IPropertyStore_GetValue (props, &k_PKEY_Device_FriendlyName, &v))
            && v.vt == VT_LPWSTR)
            wide_to_utf8 (v.pwszVal, out, cap);
        PropVariantClear (&v);
        IPropertyStore_Release (props);
    }
    if (out[0] == '\0')
        snprintf (out, (size_t) cap, "(unnamed device)");
}

static double device_rate (IMMDevice *dev)
{
    IAudioClient *ac = NULL;
    double rate = 0.0;
    if (SUCCEEDED (IMMDevice_Activate (dev, &k_IID_IAudioClient, CLSCTX_ALL,
                                       NULL, (void **) &ac)))
    {
        WAVEFORMATEX *fx = NULL;
        if (SUCCEEDED (IAudioClient_GetMixFormat (ac, &fx)) && fx != NULL)
        {
            rate = fx->nSamplesPerSec;
            CoTaskMemFree (fx);
        }
        IAudioClient_Release (ac);
    }
    return rate;
}

/* Channel count of the endpoint's shared-mode mix format — the channels a
   capture client actually receives, and so the range `inputChannel` can
   select from. */
static int device_mix_channels (IMMDevice *dev)
{
    IAudioClient *ac = NULL;
    int channels = 0;
    if (SUCCEEDED (IMMDevice_Activate (dev, &k_IID_IAudioClient, CLSCTX_ALL,
                                       NULL, (void **) &ac)))
    {
        WAVEFORMATEX *fx = NULL;
        if (SUCCEEDED (IAudioClient_GetMixFormat (ac, &fx)) && fx != NULL)
        {
            channels = fx->nChannels;
            CoTaskMemFree (fx);
        }
        IAudioClient_Release (ac);
    }
    return channels;
}

static int list_flow (IMMDeviceEnumerator *enu, EDataFlow flow, bool as_input,
                      AeDeviceInfo *out, int off, int max)
{
    IMMDeviceCollection *coll = NULL;
    if (FAILED (IMMDeviceEnumerator_EnumAudioEndpoints (enu, flow,
                                                        DEVICE_STATE_ACTIVE, &coll)))
        return off;

    char def_uid[AE_UID_MAX] = "";
    IMMDevice *def = NULL;
    if (SUCCEEDED (IMMDeviceEnumerator_GetDefaultAudioEndpoint (enu, flow,
                                                                eConsole, &def)))
    {
        device_uid (def, def_uid, sizeof (def_uid));
        IMMDevice_Release (def);
    }

    UINT n = 0;
    IMMDeviceCollection_GetCount (coll, &n);
    for (UINT i = 0; i < n && off < max; ++i)
    {
        IMMDevice *dev = NULL;
        if (FAILED (IMMDeviceCollection_Item (coll, i, &dev)))
            continue;
        AeDeviceInfo *d = &out[off];
        memset (d, 0, sizeof (*d));
        if (device_uid (dev, d->uid, sizeof (d->uid)))
        {
            device_name (dev, d->name, sizeof (d->name));
            d->nominal_rate = device_rate (dev);
            const int mix_ch = device_mix_channels (dev);
            if (as_input)
            {
                d->input_channels   = mix_ch > 0 ? mix_ch : 2;
                d->is_default_input = strcmp (d->uid, def_uid) == 0;
            }
            else
            {
                d->output_channels   = mix_ch > 0 ? mix_ch : 2;
                d->is_default_output = strcmp (d->uid, def_uid) == 0;
            }
            ++off;
        }
        IMMDevice_Release (dev);
    }
    IMMDeviceCollection_Release (coll);
    return off;
}

int ae_audio_list_devices (AeDeviceInfo **out, int *count)
{
    CoInitializeEx (NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *enu = make_enumerator();
    if (enu == NULL)
        return -1;

    AeDeviceInfo *devs = calloc (64, sizeof (AeDeviceInfo));
    if (devs == NULL)
    {
        IMMDeviceEnumerator_Release (enu);
        return -1;
    }
    int n = 0;
    n = list_flow (enu, eCapture, true, devs, n, 64);
    n = list_flow (enu, eRender, false, devs, n, 64);
    IMMDeviceEnumerator_Release (enu);
    *out = devs;
    *count = n;
    return 0;
}

static IMMDevice *find_device (IMMDeviceEnumerator *enu, EDataFlow flow,
                               const char *uid, char *err, size_t err_len)
{
    IMMDevice *dev = NULL;
    if (uid == NULL || uid[0] == '\0')
    {
        if (FAILED (IMMDeviceEnumerator_GetDefaultAudioEndpoint (enu, flow,
                                                                 eConsole, &dev)))
        {
            snprintf (err, err_len, "no default %s device",
                      flow == eCapture ? "input" : "output");
            return NULL;
        }
        return dev;
    }

    /* Match by UID within the flow's active endpoints. */
    IMMDeviceCollection *coll = NULL;
    if (SUCCEEDED (IMMDeviceEnumerator_EnumAudioEndpoints (enu, flow,
                                                           DEVICE_STATE_ACTIVE, &coll)))
    {
        UINT n = 0;
        IMMDeviceCollection_GetCount (coll, &n);
        for (UINT i = 0; i < n && dev == NULL; ++i)
        {
            IMMDevice *cand = NULL;
            if (FAILED (IMMDeviceCollection_Item (coll, i, &cand)))
                continue;
            char u[AE_UID_MAX];
            if (device_uid (cand, u, sizeof (u)) && strcmp (u, uid) == 0)
                dev = cand; /* keep the reference */
            else
                IMMDevice_Release (cand);
        }
        IMMDeviceCollection_Release (coll);
    }
    if (dev == NULL)
        snprintf (err, err_len, "%s device not found: %s",
                  flow == eCapture ? "input" : "output", uid);
    return dev;
}

/* ----------------------------------------------------------------- engine */

struct AeAudioEngine
{
    IMMDeviceEnumerator *enu;
    IMMDevice           *in_dev, *out_dev;
    IAudioClient        *in_ac, *out_ac;
    IAudioCaptureClient *cap;
    IAudioRenderClient  *ren;
    HANDLE               in_event, out_event;
    UINT32               out_total_frames;
    WinFormat            in_fmt, out_fmt;
    bool                 clients_started;

    pthread_t     in_thread, out_thread;
    bool          threads_running;
    volatile bool stopping;

    AeRing   ring;
    double   read_frac;
    bool     prefilled;
    uint64_t prefill_target;

    float *cap_scratch;    /* mono capture block */
    int    cap_channel;    /* 0-based capture channel to keep; -1 = mix all */
    int    out_channel_sel; /* 0-based playback channel for the mono fold; -1 = stereo 1-2 */
    float *proc;
    float *dry;
    _Atomic float out_peak;
    int   send_sel;    /* 0-based device channel of the record send; -1 none */
    float send_g_cur;
    float *harm_l;
    float *harm_r;

    AeCorrector corrector;

    double in_rate, out_rate;
    int    buffer_frames;
    int    latency_frames;

    char in_name[AE_NAME_MAX];
    char out_name[AE_NAME_MAX];

    /* winmm MIDI */
    HMIDIIN          midi_in[8];
    int              midi_in_count;
    _Atomic uint64_t hw_midi_lo;
    _Atomic uint64_t hw_midi_hi;

    AeAtomicParams params;
};

/* ------------------------------------------------------------------- MIDI */

int ae_audio_list_midi_sources (char out[][AE_NAME_MAX], int max)
{
    const UINT n = midiInGetNumDevs();
    int m = 0;
    for (UINT i = 0; i < n && m < max; ++i)
    {
        MIDIINCAPSA caps;
        if (midiInGetDevCapsA (i, &caps, sizeof (caps)) == MMSYSERR_NOERROR)
        {
            snprintf (out[m], AE_NAME_MAX, "%s", caps.szPname);
            ++m;
        }
    }
    return m;
}

static void CALLBACK midi_cb (HMIDIIN h, UINT msg, DWORD_PTR inst,
                              DWORD_PTR p1, DWORD_PTR p2)
{
    (void) h; (void) p2;
    if (msg != MIM_DATA)
        return;
    AeAudioEngine *e = (AeAudioEngine *) inst;
    const unsigned status = (unsigned) (p1 & 0xFF);
    const int      note   = (int) ((p1 >> 8) & 0x7F);
    const int      vel    = (int) ((p1 >> 16) & 0x7F);
    const unsigned hi     = status & 0xF0;

    bool on;
    if (hi == 0x90 && vel > 0)
        on = true;
    else if (hi == 0x80 || (hi == 0x90 && vel == 0))
        on = false;
    else
        return;

    _Atomic uint64_t *word = note < 64 ? &e->hw_midi_lo : &e->hw_midi_hi;
    const uint64_t bit = 1ull << (note & 63);
    if (on)
        atomic_fetch_or_explicit (word, bit, memory_order_relaxed);
    else
        atomic_fetch_and_explicit (word, ~bit, memory_order_relaxed);
}

/* Non-fatal, like the mac backend: audio runs fine without MIDI. */
static void midi_setup (AeAudioEngine *e, const char *source_name)
{
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n && e->midi_in_count < 8; ++i)
    {
        if (source_name != NULL && source_name[0] != '\0')
        {
            MIDIINCAPSA caps;
            if (midiInGetDevCapsA (i, &caps, sizeof (caps)) != MMSYSERR_NOERROR
                || strcmp (caps.szPname, source_name) != 0)
                continue; /* a named source: connect only that one */
        }
        HMIDIIN h = NULL;
        if (midiInOpen (&h, i, (DWORD_PTR) midi_cb, (DWORD_PTR) e,
                        CALLBACK_FUNCTION) == MMSYSERR_NOERROR)
        {
            if (midiInStart (h) == MMSYSERR_NOERROR)
                e->midi_in[e->midi_in_count++] = h;
            else
                midiInClose (h);
        }
    }
}

/* ---------------------------------------------------------------- threads */

static void *capture_thread (void *arg)
{
    AeAudioEngine *e = arg;
    CoInitializeEx (NULL, COINIT_MULTITHREADED);

    while (! e->stopping)
    {
        WaitForSingleObject (e->in_event, 100);
        UINT32 pkt = 0;
        while (! e->stopping
               && IAudioCaptureClient_GetNextPacketSize (e->cap, &pkt) == S_OK
               && pkt > 0)
        {
            BYTE  *data = NULL;
            UINT32 frames = 0;
            DWORD  flags = 0;
            if (FAILED (IAudioCaptureClient_GetBuffer (e->cap, &data, &frames,
                                                       &flags, NULL, NULL)))
                break;

            UINT32 done = 0;
            while (done < frames)
            {
                UINT32 n = frames - done;
                if (n > MAX_FRAMES)
                    n = MAX_FRAMES;
                const int ch  = e->in_fmt.channels;
                const int sel = e->cap_channel; /* -1 = mix all channels */
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == NULL)
                {
                    memset (e->cap_scratch, 0, n * sizeof (float));
                }
                else if (e->in_fmt.is_float)
                {
                    const float *s = (const float *) data + (size_t) done * ch;
                    if (sel >= 0)
                        for (UINT32 i = 0; i < n; ++i)
                            e->cap_scratch[i] = s[i * ch + sel];
                    else
                        for (UINT32 i = 0; i < n; ++i)
                        {
                            float m = 0.0f;
                            for (int c = 0; c < ch; ++c)
                                m += s[i * ch + c];
                            e->cap_scratch[i] = m / (float) ch;
                        }
                }
                else /* 16-bit PCM */
                {
                    const short *s = (const short *) data + (size_t) done * ch;
                    if (sel >= 0)
                        for (UINT32 i = 0; i < n; ++i)
                            e->cap_scratch[i] = (float) s[i * ch + sel] / 32768.0f;
                    else
                        for (UINT32 i = 0; i < n; ++i)
                        {
                            int m = 0;
                            for (int c = 0; c < ch; ++c)
                                m += s[i * ch + c];
                            e->cap_scratch[i] = (float) m / (float) ch / 32768.0f;
                        }
                }
                ae_ring_write (&e->ring, e->cap_scratch, n);
                done += n;
            }
            IAudioCaptureClient_ReleaseBuffer (e->cap, frames);
        }
    }
    CoUninitialize();
    return NULL;
}

static void render_block (AeAudioEngine *e, BYTE *out, UINT32 n)
{
    /* Pop from the ring with resampling + drift compensation (same logic as
       the CoreAudio render callback). */
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
            e->prefilled = false;
    }
    for (int i = got; i < (int) n; ++i)
        e->proc[i] = 0.0f;

    memcpy (e->dry, e->proc, n * sizeof (float));

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

    /* Lead mute is harmony-only output; bypass still wins (a dry passthrough
       with the lead "muted" would be silence). */
    const float *src = bypass ? e->dry : e->proc;
    const float  lg  = bypass ? 1.0f : (lead ? lead_g : 0.0f);
    const int    ch  = e->out_fmt.channels;
    const int    sel = e->out_channel_sel; /* -1 = stereo on channels 1-2 */
    const int    ssel = e->send_sel;       /* record send channel, -1 = none */
    const float *wet  = ae_corrector_lead_wet (&e->corrector);
    const float  send_target = mix.send_on ? mix.send_gain : 0.0f;
    float        sg = e->send_g_cur;

#define AE_SEND_VALUE(i) \
    (mix.send_content == AE_SEND_WET  ? (wet ? wet[i] : 0.0f) \
                                        + 0.5f * (e->harm_l[i] + e->harm_r[i]) \
   : mix.send_content == AE_SEND_LEAD ? (wet ? wet[i] : 0.0f) \
   : mix.send_content == AE_SEND_HARM ? 0.5f * (e->harm_l[i] + e->harm_r[i]) \
   : (bypass ? e->dry[i] \
             : lg * src[i] + 0.5f * (e->harm_l[i] + e->harm_r[i])) * gain)

    if (e->out_fmt.is_float)
    {
        float *d = (float *) out;
        float pk = atomic_load_explicit (&e->out_peak, memory_order_relaxed) * 0.98f;
        for (UINT32 i = 0; i < n; ++i)
        {
            const float l  = (lg * src[i] + (bypass ? 0.0f : e->harm_l[i])) * gain;
            const float r  = (lg * src[i] + (bypass ? 0.0f : e->harm_r[i])) * gain;
            const float ppk = fabsf (l) > fabsf (r) ? fabsf (l) : fabsf (r);
            if (ppk > pk) pk = ppk;
            const float lc = ae_soft_clip (l), rc = ae_soft_clip (r);
            const float mc = ae_soft_clip (0.5f * (l + r));
            sg += (send_target - sg) * 0.0005f;
            for (int c = 0; c < ch; ++c)
                d[i * ch + c] = c == ssel ? ae_soft_clip (AE_SEND_VALUE (i) * sg)
                              : sel >= 0 ? (c == sel ? mc : 0.0f)
                              : ch >= 2 ? (c == 0 ? lc : (c == 1 ? rc : mc))
                                        : mc;
        }
        atomic_store_explicit (&e->out_peak, pk, memory_order_relaxed);
    }
    else /* 16-bit PCM */
    {
        short *d = (short *) out;
        float pk = atomic_load_explicit (&e->out_peak, memory_order_relaxed) * 0.98f;
        for (UINT32 i = 0; i < n; ++i)
        {
            const float l  = (lg * src[i] + (bypass ? 0.0f : e->harm_l[i])) * gain;
            const float r  = (lg * src[i] + (bypass ? 0.0f : e->harm_r[i])) * gain;
            const float ppk = fabsf (l) > fabsf (r) ? fabsf (l) : fabsf (r);
            if (ppk > pk) pk = ppk;
            const float lc = ae_soft_clip (l), rc = ae_soft_clip (r);
            const float mc = ae_soft_clip (0.5f * (l + r));
            sg += (send_target - sg) * 0.0005f;
            for (int c = 0; c < ch; ++c)
            {
                const float v = c == ssel ? ae_soft_clip (AE_SEND_VALUE (i) * sg)
                              : sel >= 0 ? (c == sel ? mc : 0.0f)
                              : ch >= 2 ? (c == 0 ? lc : (c == 1 ? rc : mc))
                                        : mc;
                d[i * ch + c] = (short) lrintf (v * 32767.0f);
            }
        }
        atomic_store_explicit (&e->out_peak, pk, memory_order_relaxed);
    }
#undef AE_SEND_VALUE
    e->send_g_cur = sg;
}

static void *render_thread (void *arg)
{
    AeAudioEngine *e = arg;
    CoInitializeEx (NULL, COINIT_MULTITHREADED);

    while (! e->stopping)
    {
        WaitForSingleObject (e->out_event, 100);
        UINT32 padding = 0;
        if (FAILED (IAudioClient_GetCurrentPadding (e->out_ac, &padding)))
            continue;
        UINT32 avail = e->out_total_frames - padding;
        while (avail > 0 && ! e->stopping)
        {
            UINT32 n = avail;
            if (n > MAX_FRAMES)
                n = MAX_FRAMES;
            BYTE *buf = NULL;
            if (FAILED (IAudioRenderClient_GetBuffer (e->ren, n, &buf)))
                break;
            render_block (e, buf, n);
            IAudioRenderClient_ReleaseBuffer (e->ren, n, 0);
            avail -= n;
        }
    }
    CoUninitialize();
    return NULL;
}

/* -------------------------------------------------------------- lifecycle */

static void engine_teardown (AeAudioEngine *e)
{
    e->stopping = true;
    if (e->in_event != NULL)  SetEvent (e->in_event);
    if (e->out_event != NULL) SetEvent (e->out_event);
    if (e->threads_running)
    {
        pthread_join (e->in_thread, NULL);
        pthread_join (e->out_thread, NULL);
    }
    for (int i = 0; i < e->midi_in_count; ++i)
    {
        midiInStop (e->midi_in[i]);
        midiInClose (e->midi_in[i]);
    }
    if (e->clients_started)
    {
        IAudioClient_Stop (e->in_ac);
        IAudioClient_Stop (e->out_ac);
    }
    if (e->cap != NULL)     IAudioCaptureClient_Release (e->cap);
    if (e->ren != NULL)     IAudioRenderClient_Release (e->ren);
    if (e->in_ac != NULL)   IAudioClient_Release (e->in_ac);
    if (e->out_ac != NULL)  IAudioClient_Release (e->out_ac);
    if (e->in_dev != NULL)  IMMDevice_Release (e->in_dev);
    if (e->out_dev != NULL) IMMDevice_Release (e->out_dev);
    if (e->enu != NULL)     IMMDeviceEnumerator_Release (e->enu);
    if (e->in_event != NULL)  CloseHandle (e->in_event);
    if (e->out_event != NULL) CloseHandle (e->out_event);
    ae_ring_free (&e->ring);
    free (e->cap_scratch);
    free (e->proc);
    free (e->dry);
    free (e->harm_l);
    free (e->harm_r);
    ae_corrector_free (&e->corrector);
    free (e);
}

void ae_audio_engine_set_params (AeAudioEngine *e, const AeLiveParams *p)
{
    if (e != NULL)
        ae_atomic_params_store (&e->params, p);
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
    out->detected_hz     = ae_corrector_detected_hz (&e->corrector);
    out->target_hz       = ae_corrector_target_hz (&e->corrector);
    out->shift_st        = ae_corrector_shift_st (&e->corrector);
    out->shift_st_min    = ae_corrector_shift_st_min (&e->corrector);
    out->shift_st_max    = ae_corrector_shift_st_max (&e->corrector);
    out->lead_makeup     = ae_corrector_lead_makeup (&e->corrector);
    out->out_peak        = atomic_load_explicit (&e->out_peak, memory_order_relaxed);
    out->voiced          = ae_corrector_voiced (&e->corrector);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        out->harm_deg[v] = ae_corrector_harm_degree (&e->corrector, v);
    out->trace_len = ae_corrector_trace (&e->corrector, &out->trace_seq,
                                         out->trace_det, out->trace_tgt, AE_TRACE_MAX);
    out->midi_held_lo = atomic_load_explicit (&e->hw_midi_lo, memory_order_relaxed)
                      | atomic_load_explicit (&e->params.vmidi_lo, memory_order_relaxed);
    out->midi_held_hi = atomic_load_explicit (&e->hw_midi_hi, memory_order_relaxed)
                      | atomic_load_explicit (&e->params.vmidi_hi, memory_order_relaxed);
    snprintf (out->input_name,  sizeof (out->input_name),  "%s", e->in_name);
    snprintf (out->output_name, sizeof (out->output_name), "%s", e->out_name);
}

/* Initialize one shared-mode client. Returns false with err filled. */
static bool init_client (IMMDevice *dev, bool capture, int buffer_frames,
                         IAudioClient **out_ac, WinFormat *out_fmt,
                         HANDLE *out_event, char *err, size_t err_len)
{
    IAudioClient *ac = NULL;
    if (FAILED (IMMDevice_Activate (dev, &k_IID_IAudioClient, CLSCTX_ALL,
                                    NULL, (void **) &ac)))
    {
        snprintf (err, err_len, "IAudioClient activate failed");
        return false;
    }

    WAVEFORMATEX *fx = NULL;
    if (FAILED (IAudioClient_GetMixFormat (ac, &fx)) || fx == NULL)
    {
        snprintf (err, err_len, "GetMixFormat failed");
        IAudioClient_Release (ac);
        return false;
    }
    if (! parse_format (fx, out_fmt))
    {
        snprintf (err, err_len, "unsupported mix format (tag %d, %d bits)",
                  fx->wFormatTag, fx->wBitsPerSample);
        CoTaskMemFree (fx);
        IAudioClient_Release (ac);
        return false;
    }

    /* Buffer: at least 3 requested blocks and at least 30 ms. */
    double sec = 3.0 * buffer_frames / (double) out_fmt->rate;
    if (sec < 0.030)
        sec = 0.030;
    const REFERENCE_TIME dur = (REFERENCE_TIME) (sec * 1.0e7);

    const HRESULT hr = IAudioClient_Initialize (ac, AUDCLNT_SHAREMODE_SHARED,
                                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                dur, 0, fx, NULL);
    CoTaskMemFree (fx);
    if (FAILED (hr))
    {
        snprintf (err, err_len, "%s Initialize failed (0x%08lx)%s",
                  capture ? "capture" : "render", (unsigned long) hr,
                  capture ? " — check microphone privacy settings" : "");
        IAudioClient_Release (ac);
        return false;
    }

    HANDLE ev = CreateEventA (NULL, FALSE, FALSE, NULL);
    if (ev == NULL || FAILED (IAudioClient_SetEventHandle (ac, ev)))
    {
        snprintf (err, err_len, "SetEventHandle failed");
        if (ev != NULL)
            CloseHandle (ev);
        IAudioClient_Release (ac);
        return false;
    }

    *out_ac = ac;
    *out_event = ev;
    return true;
}

AeAudioEngine *ae_audio_engine_start (const AeEngineConfig *cfg, char *err, size_t err_len)
{
    err[0] = '\0';
    CoInitializeEx (NULL, COINIT_MULTITHREADED);

    AeAudioEngine *e = calloc (1, sizeof (*e));
    if (e == NULL)
    {
        snprintf (err, err_len, "out of memory");
        return NULL;
    }

    e->enu = make_enumerator();
    if (e->enu == NULL)
    {
        snprintf (err, err_len, "MMDeviceEnumerator create failed");
        engine_teardown (e);
        return NULL;
    }

    e->in_dev = find_device (e->enu, eCapture, cfg->input_uid, err, err_len);
    if (e->in_dev == NULL)
    {
        engine_teardown (e);
        return NULL;
    }
    e->out_dev = find_device (e->enu, eRender, cfg->output_uid, err, err_len);
    if (e->out_dev == NULL)
    {
        engine_teardown (e);
        return NULL;
    }
    device_name (e->in_dev,  e->in_name,  sizeof (e->in_name));
    device_name (e->out_dev, e->out_name, sizeof (e->out_name));

    e->buffer_frames = cfg->buffer_frames >= 32 && cfg->buffer_frames <= 2048
                         ? cfg->buffer_frames : 256;

    if (! init_client (e->in_dev, true, e->buffer_frames,
                       &e->in_ac, &e->in_fmt, &e->in_event, err, err_len)
        || ! init_client (e->out_dev, false, e->buffer_frames,
                          &e->out_ac, &e->out_fmt, &e->out_event, err, err_len))
    {
        engine_teardown (e);
        return NULL;
    }
    e->in_rate  = e->in_fmt.rate;
    e->out_rate = e->out_fmt.rate;

    /* Bind a specific capture channel of the shared-mode mix format; the
       default keeps the old mix-to-mono behaviour. */
    e->cap_channel = -1;
    if (cfg->input_channel > 0)
    {
        if (cfg->input_channel > e->in_fmt.channels)
        {
            snprintf (err, err_len, "device \"%s\" has no input channel %d (%d available)",
                      e->in_name, cfg->input_channel, e->in_fmt.channels);
            engine_teardown (e);
            return NULL;
        }
        e->cap_channel = cfg->input_channel - 1;
    }
    /* Same for playback: a bound channel gets the mono fold, the rest get
       silence; the default keeps stereo on the first two. */
    e->out_channel_sel = -1;
    if (cfg->output_channel > 0)
    {
        if (cfg->output_channel > e->out_fmt.channels)
        {
            snprintf (err, err_len, "device \"%s\" has no output channel %d (%d available)",
                      e->out_name, cfg->output_channel, e->out_fmt.channels);
            engine_teardown (e);
            return NULL;
        }
        e->out_channel_sel = cfg->output_channel - 1;
    }
    e->send_sel = -1;
    e->send_g_cur = 0.0f;
    if (cfg->send_channel > 0)
    {
        if (cfg->send_channel > e->out_fmt.channels)
        {
            snprintf (err, err_len, "device \"%s\" has no output channel %d for the record send (%d available)",
                      e->out_name, cfg->send_channel, e->out_fmt.channels);
            engine_teardown (e);
            return NULL;
        }
        e->send_sel = cfg->send_channel - 1;
    }

    if (FAILED (IAudioClient_GetService (e->in_ac, &k_IID_IAudioCaptureClient,
                                         (void **) &e->cap))
        || FAILED (IAudioClient_GetService (e->out_ac, &k_IID_IAudioRenderClient,
                                            (void **) &e->ren))
        || FAILED (IAudioClient_GetBufferSize (e->out_ac, &e->out_total_frames)))
    {
        snprintf (err, err_len, "WASAPI service setup failed");
        engine_teardown (e);
        return NULL;
    }

    if (ae_ring_init (&e->ring, 1 << 16) != 0)
    {
        snprintf (err, err_len, "out of memory");
        engine_teardown (e);
        return NULL;
    }
    e->cap_scratch = calloc (MAX_FRAMES, sizeof (float));
    e->proc        = calloc (MAX_FRAMES, sizeof (float));
    e->dry         = calloc (MAX_FRAMES, sizeof (float));
    e->harm_l      = calloc (MAX_FRAMES, sizeof (float));
    e->harm_r      = calloc (MAX_FRAMES, sizeof (float));
    if (e->cap_scratch == NULL || e->proc == NULL || e->dry == NULL
        || e->harm_l == NULL || e->harm_r == NULL)
    {
        snprintf (err, err_len, "out of memory");
        engine_teardown (e);
        return NULL;
    }

    ae_corrector_prepare (&e->corrector, e->out_rate, MAX_FRAMES,
                          cfg->det_min_hz, cfg->det_max_hz, cfg->quality);
    ae_audio_engine_set_params (e, &cfg->params);

    uint64_t cushion = (uint64_t) (e->in_rate * 0.020);
    const uint64_t three_blocks = (uint64_t) (3.0 * e->buffer_frames * e->in_rate / e->out_rate);
    if (cushion < three_blocks)
        cushion = three_blocks;
    e->prefill_target = cushion;

    e->latency_frames = ae_corrector_latency (&e->corrector)
                      + (int) ((double) cushion * e->out_rate / e->in_rate)
                      + e->buffer_frames;

    if (pthread_create (&e->in_thread, NULL, capture_thread, e) != 0)
    {
        snprintf (err, err_len, "thread create failed");
        engine_teardown (e);
        return NULL;
    }
    if (pthread_create (&e->out_thread, NULL, render_thread, e) != 0)
    {
        e->stopping = true;
        SetEvent (e->in_event);
        pthread_join (e->in_thread, NULL);
        snprintf (err, err_len, "thread create failed");
        engine_teardown (e);
        return NULL;
    }
    e->threads_running = true;

    if (FAILED (IAudioClient_Start (e->in_ac)) || FAILED (IAudioClient_Start (e->out_ac)))
    {
        snprintf (err, err_len, "WASAPI start failed — check microphone "
                                "privacy settings (Settings > Privacy > Microphone)");
        e->clients_started = false;
        engine_teardown (e);
        return NULL;
    }
    e->clients_started = true;

    midi_setup (e, cfg->midi_source); /* non-fatal */

    return e;
}

void ae_audio_engine_stop (AeAudioEngine *e)
{
    if (e != NULL)
        engine_teardown (e);
}
