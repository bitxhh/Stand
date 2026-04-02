# Stand — SDR Receiver for LimeSDR

Real-time SDR receiver application with WBFM radio, spectrum display, and I/Q recording.

Built with C++/Qt6, targeting LimeSDR USB hardware via LimeSuite.

## Features

- **FM broadcast receiver** — full WBFM demodulation chain with audio playback
- **Real-time spectrum display** — FFT with EMA smoothing, zoom/pan, dark theme
- **VFO tuner** — click spectrum or enter frequency to tune FM stations in-band
- **Raw I/Q recording** — dump int16 interleaved samples to .raw file
- **Bandpass WAV export** — NCO shift + FIR filter + decimate to stereo float32 WAV
- **Signal quality indicator** — real-time SNR meter (NOISE / MARGINAL / SIGNAL)

## Architecture

```
                      ┌─────────────────────────────────┐
                      │         DeviceDetailWindow       │  Main thread
                      │   (spectrum plot, FM controls)   │  (Qt event loop)
                      └──────────┬──────────────────────┘
                                 │ Qt signals
                      ┌──────────┴──────────────────────┐
                      │       DeviceController           │
                      │   (UI → IDevice command layer)   │
                      └──────────┬──────────────────────┘
                                 │
              ┌──────────────────┴───────────────────────┐
              │              StreamWorker                 │  Worker thread
              │         (QThread, I/Q recv loop)          │
              └──────────────────┬───────────────────────┘
                                 │
              ┌──────────────────┴───────────────────────┐
              │               Pipeline                    │
              │      (broadcasts I/Q to handlers)         │
              └───┬──────────┬──────────┬──────────┬─────┘
                  │          │          │          │
           ┌─────┴───┐ ┌────┴────┐ ┌───┴───┐ ┌───┴────────┐
           │FftHandler│ │FmDemod  │ │RawFile│ │Bandpass    │
           │ spectrum │ │Handler  │ │Handler│ │Handler     │
           │ display  │ │ → audio │ │ → .raw│ │ → .wav     │
           └─────────┘ └────┬────┘ └───────┘ └────────────┘
                            │
                    ┌───────┴────────┐
                    │ FmAudioOutput  │  Main thread
                    │ resample + AGC │
                    │ → WASAPI sink  │
                    └────────────────┘
```

## FM DSP Chain

```
int16 I/Q  ──►  DC blocker  ──►  NCO freq-shift  ──►  FIR1 LPF (255 taps)
           ──►  decimate D1  ──►  IF @ 500 kHz
           ──►  FM discriminator (atan2)
           ──►  de-emphasis IIR (50/75 us)
           ──►  FIR2 LPF (255 taps, 15 kHz)
           ──►  decimate D2=10  ──►  audio @ 50 kHz
           ──►  resample → 48 kHz  ──►  AGC  ──►  speakers
```

## Build

### Requirements

- **CMake** 3.20+
- **MinGW** (GCC 13+)
- **Qt 6.10** — Widgets, Concurrent, PrintSupport, Multimedia
- **LimeSuite** — LimeSDR driver library
- **FFTW3** — double precision (bundled in `external/FFTW/`)
- **QCustomPlot** — bundled in `external/qcustomplot/`

### Build commands

```bash
# Release (recommended for FM listening)
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target Stand

# Debug (limited to 31 FIR taps, max ~10 MS/s)
cmake -B build-debug -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --target Stand
```

### Run

Connect a LimeSDR USB, then launch `build/Stand.exe`. Select your device from the list, initialize, set sample rate, calibrate, and start streaming.

## Hardware Configuration

The LimeSDR is configured to match **ExtIO_LimeSDR** (the HDSDR plugin) — a known-good reference for clean FM reception:

- Analog LPF bandwidth = sample rate
- TIA protected around `LMS_SetLPFBW` calls (LimeSuite bug workaround)
- PGA compensation register (`RCC_CTL_PGA_RBB`) updated on every gain change
- Calibration bandwidth = max(sample rate, 2.5 MHz)

## Project Structure

```
Core/           Interfaces (IDevice, IPipelineHandler, Pipeline, Logger)
Hardware/       LimeSDR implementation (LimeDevice, StreamWorker, DeviceController)
DSP/            Signal processing (FFT, FM demodulator, bandpass filter, raw recorder)
Audio/          Audio output (resampler, AGC, QAudioSink wrapper)
Application/    Qt UI (device selection, control panel, spectrum plot)
Tests/          Unit tests (Catch2)
external/       Bundled dependencies (FFTW, QCustomPlot)
```

See [CLAUDE.md](CLAUDE.md) for detailed architecture documentation, DSP parameters, and hardware configuration rules.

## License

This project is provided as-is for educational and research purposes.
