# Vendored dependencies

## signalsmith-stretch

[Signalsmith Stretch](https://signalsmith-audio.co.uk/code/stretch/) — the
phase-vocoder pitch/time library used for all pitch shifting (the corrected
voice and every harmony voice). MIT licensed.

**Vendored: Stretch 1.3.2** plus the two headers it needs from
[signalsmith-linear](https://github.com/Signalsmith-Audio/linear) (`stft.h`,
`fft.h`, and the optional `platform/` FFT backends), so the tree builds with
no network access and no submodules:

```
signalsmith-stretch/
├── signalsmith-stretch.h       Stretch 1.3.2
├── signalsmith-linear/         its FFT/STFT dependency
│   ├── stft.h  fft.h
│   └── platform/               optional faster FFT backends (see below)
├── LICENSE-stretch.txt
└── LICENSE-linear.txt
```

Only `src/shifter.cpp` includes any of this; the rest of the app talks to
the C ABI in `src/shifter.h`. The headers are included with `-isystem`, so
upstream warnings stay out of our build.

### Formants

1.3 added formant shifting/compensation, which the app uses to hold formants
still while the pitch moves (otherwise transposed harmony voices chipmunk).
`src/shifter.cpp` reaches those methods through a compile-time capability
probe rather than a version test, so an older header still compiles — it
just silently loses formant control. `ae_shifter_has_formant_support()` and
the `formantSupport` field in `/api/status` report which state you are in.

### Faster FFTs (not enabled)

`platform/` carries the library's optional backends. Defining
`SIGNALSMITH_USE_ACCELERATE` (macOS), `SIGNALSMITH_USE_IPP`, or
`SIGNALSMITH_USE_PFFFT` in `CXXFLAGS` switches the FFT over. The portable
default is fast enough here (six concurrent voices cost roughly 8% of one
core), so none is enabled — and none has been tested in this tree.
