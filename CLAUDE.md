# CLAUDE.md

Stand — SDR receiver for LimeSDR. Real-time spectrum, WBFM/AM demodulation, I/Q recording, WAV export. **v1.1.0**

## Build

CMake + MinGW + Qt6.

```bash
cmake --build cmake-build-release-mingw-qt --target Stand
```

Use **Release** for FM listening — Debug uses 31-tap FIR1 and can't sustain ≥15 MS/s.

## Dependencies

| Dependency | Location |
|------------|----------|
| Qt 6.10 | `C:/Qt/6.10.0/mingw_64` — Widgets, Concurrent, PrintSupport, Multimedia |
| LimeSuite | `C:/LimeSuite` — headers + `LimeSuite.dll` |
| FFTW3 | `external/FFTW/` — double precision, static |
| QCustomPlot | `external/qcustomplot/` — static lib |
| Catch2 v3.7.1 | `Tests/` — unit tests for FFT, FM, AM |

## Quick reference

- Architecture, threading, interfaces, data flow → `docs/architecture.md`
- FM/AM DSP chains and parameters → `docs/dsp.md`
- LimeSDR init, hardware quirks, gain structure → `docs/hardware.md`
- Load all docs in conversation → `/stand-docs`
