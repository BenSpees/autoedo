# Vendored dependencies

## signalsmith-stretch

[Signalsmith Stretch](https://signalsmith-audio.co.uk/code/stretch/) — the
phase-vocoder pitch/time library used for all pitch shifting (the corrected
voice and every harmony voice). MIT licensed; see `LICENSE-stretch.txt` and
`LICENSE-dsp.txt`.

**Vendored version: 1.1.1**, together with the `signalsmith-audio/dsp`
headers it depends on (`dsp/`), so the tree builds with no network access
and no submodules. Only `src/shifter.cpp` includes it; everything else in
the app talks to the C ABI in `src/shifter.h`.

### Upgrading to 1.3.x

1.3 adds **formant shifting/compensation**, which is the one feature the app
is written for but cannot use yet: without it, large harmony intervals move
the formants with the pitch (the "chipmunk" effect). The shim already calls
`setFormantSemitones()` behind a compile-time capability probe, so the
feature switches on by itself once a header that has it is dropped in —
`ae_shifter_has_formant_support()` and the `formantSupport` field in
`/api/status` report which state you are in.

1.3 replaced the bundled `dsp/` dependency with
[signalsmith-linear](https://github.com/Signalsmith-Audio/linear) (for FFTs),
which is *not* included here. To upgrade:

```
third_party/signalsmith-stretch/
├── signalsmith-stretch.h      ← from Signalsmith-Audio/signalsmith-stretch
└── signalsmith-linear/        ← from Signalsmith-Audio/linear (tag 0.3.1)
    └── stft.h  (+ its headers)
```

then delete `dsp/` and rebuild — `src/shifter.cpp` and the Makefile need no
changes (`CXXFLAGS` already puts `third_party/` on the include path).
