# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

**Stand** is an SDR (Software-Defined Radio) receiver application for LimeSDR hardware. It provides real-time spectrum display, WBFM radio demodulation with audio playback, raw I/Q recording, and bandpass-filtered WAV export.

Current version: **v1.0.0** (working FM broadcast receiver).

## Build

CMake + MinGW + Qt6. Build from CLion using `cmake-build-debug-mingw-qt` or `cmake-build-release-mingw-qt` profiles.

```bash
cmake --build cmake-build-release-mingw-qt --target Stand
```

**Important:** Use Release for FM listening. Debug builds use 31-tap FIR1 (vs 255 in Release) and cannot sustain sample rates >= 15 MS/s without USB FIFO overflow.

## Dependencies

| Dependency | Location | Notes |
|------------|----------|-------|
| Qt 6.10 | `C:/Qt/6.10.0/mingw_64` | Widgets, Concurrent, PrintSupport, Multimedia |
| LimeSuite | `C:/LimeSuite` | Headers + `LimeSuite.dll` |
| FFTW3 | `external/FFTW/` | Double precision, static import |
| QCustomPlot | `external/qcustomplot/` | Bundled source, built as static lib |
| Catch2 v3.7.1 | `Tests/` | Unit tests for FFT and FM demodulator |

## Architecture

### Component diagram

```
main.cpp
  ├── LimeDeviceManager          — scans USB for LimeSDR devices
  └── Application                — QApplication + DeviceSelectionWindow
        └── DeviceDetailWindow   — main UI: 3-page SDR control panel
              ├── DeviceController     — thin command layer (UI → IDevice), no widgets
              ├── StreamWorker         — dedicated QThread: I/Q recv loop
              │     └── Pipeline       — broadcasts I/Q blocks to registered handlers:
              │           ├── FftHandler       → spectrum display (30 fps)
              │           ├── FmDemodHandler   → WBFM demod → audio
              │           ├── RawFileHandler   → .raw I/Q dump
              │           └── BandpassHandler  → filtered WAV export
              └── FmAudioOutput        — resampler + AGC + QAudioSink (WASAPI)
```

### Directory layout

```
Core/               Interfaces and infrastructure
  ├── IDevice.h           SDR-agnostic device interface
  ├── IDeviceManager.h    Device discovery interface
  ├── IPipelineHandler.h  Signal processing handler interface
  ├── Pipeline.h/.cpp     I/Q block router (snapshot-lock pattern)
  ├── Logger.h/.cpp       Thread-safe singleton logger
  └── LimeException.h     Exception hierarchy for LimeSuite errors

Hardware/           LimeSDR implementation
  ├── LimeDevice.h/.cpp          IDevice for LimeSDR (LimeSuite C API)
  ├── LimeDeviceManager.h/.cpp   IDeviceManager, USB device scanning
  ├── DeviceController.h/.cpp    Exception-safe UI→device command wrapper
  └── StreamWorker.h/.cpp        QThread I/Q loop, pipeline dispatch

DSP/                Signal processing
  ├── FftProcessor.h/.cpp        Stateless FFT (FFTW3, thread-local plan cache)
  ├── FftHandler.h/.cpp          IPipelineHandler: throttled FFT + EMA smoothing
  ├── FmDemodulator.h/.cpp       Stateful WBFM demodulator (full DSP chain)
  ├── FmDemodHandler.h/.cpp      IPipelineHandler wrapper for FmDemodulator
  ├── BandpassExporter.h/.cpp    NCO + FIR + decimate → WAV writer
  ├── BandpassHandler.h/.cpp     IPipelineHandler wrapper for BandpassExporter
  └── RawFileHandler.h/.cpp      IPipelineHandler: raw int16 I/Q dump

Audio/              Audio output
  └── FmAudioOutput.h/.cpp       Linear resampler + AGC + QAudioSink

Application/        UI (Qt widgets only — no DSP, no hardware calls)
  ├── Application.h/.cpp         DeviceSelectionWindow + DeviceDetailWindow
  └── (3 pages: Device Info, Device Control, FFT/Radio)
```

### Threading model

| Thread | Components | Responsibilities |
|--------|-----------|-----------------|
| **Main (Qt event loop)** | All widgets, DeviceController, FmAudioOutput | UI updates, audio sink writes, device commands |
| **StreamWorker (QThread)** | StreamWorker, Pipeline, all IPipelineHandlers | I/Q recv, FFT, FM demod, file I/O |

All cross-thread communication uses `Qt::QueuedConnection` signals. No shared mutable state except atomics for pending parameter updates.

### Key interfaces

**IDevice** — SDR-agnostic device abstraction:
- `init()` / `calibrate()` / `setSampleRate()` / `setFrequency()` / `setGain()`
- `startStream()` / `readBlock()` / `stopStream()` (called from worker thread)
- State machine: `Disconnected → Connected → Ready → Streaming → Error`
- Emits `stateChanged(DeviceState)`

**IPipelineHandler** — signal processing stage:
```cpp
virtual void processBlock(const int16_t* iq, int count, double sampleRateHz) = 0;
virtual void onStreamStarted(double sampleRateHz) {}
virtual void onStreamStopped() {}
```
Called synchronously in StreamWorker thread. Must not block (budget: ~4 ms at 4 MS/s).

**Pipeline** — snapshot-lock router:
- `addHandler()` / `removeHandler()` thread-safe (mutex)
- `dispatchBlock()` copies handler list under lock, calls handlers without lock (no stream stall)

## Data flow

### I/Q → Spectrum display

```
LimeDevice::readBlock()  →  StreamWorker  →  Pipeline  →  FftHandler
    int16[16384×2]              ↓                            ↓
                          100ms timeout               Throttle 30 fps
                                                     FftProcessor::process()
                                                      ├─ Hann window
                                                      ├─ FFTW forward
                                                      ├─ FFT-shift (DC center)
                                                      ├─ Power → dBFS
                                                      └─ EMA smooth (α=0.1)
                                                            ↓
                                                     emit fftReady(frame)
                                                            ↓ QueuedConnection
                                                     QCustomPlot::replot()
```

### I/Q → FM audio

```
LimeDevice::readBlock()  →  Pipeline  →  FmDemodHandler  →  FmDemodulator
    int16[16384×2]                                              ↓
                                                    ┌───────────────────────┐
                                                    │ DC blocker (IIR HP)   │
                                                    │ NCO freq-shift        │
                                                    │ FIR1 LPF (255 taps)  │ ← push O(1) per sample
                                                    │ Decimate D1           │ ← compute O(N) only here
                                                    │ FM discriminator      │
                                                    │ De-emphasis IIR       │
                                                    │ FIR2 LPF (255 taps)  │
                                                    │ Decimate D2=10        │
                                                    └───────────────────────┘
                                                              ↓
                                                    QVector<float> @ 50 kHz
                                                              ↓ emit audioReady()
                                                              ↓ QueuedConnection
                                                    FmAudioOutput::push()
                                                      ├─ Linear resample → 48 kHz
                                                      ├─ AGC (target 0.12 RMS)
                                                      ├─ Mono → stereo
                                                      └─ QAudioSink (WASAPI)
```

## FM demodulation DSP chain

```
int16 I/Q → DC blocker → NCO shift → FIR1 LPF (complex, 255 taps)
         → decimate D1 → IF @ 500 kHz
         → FM discriminator (atan2 conjugate product)
         → de-emphasis IIR (τ = 50 µs EU / 75 µs US)
         → FIR2 LPF (real, 255 taps, fc ≈ 15 kHz)
         → decimate D2 = 10 → audio @ 50 kHz
```

### Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| IF target | 500 kHz | D1 = round(inputSR / 500000) |
| Audio SR | 50 kHz | IF / D2, always exactly 50 kHz |
| FIR1 taps | 255 (Release) / 31 (Debug) | Blackman window, -55 dB at Nyquist |
| FIR1 bandwidth | 150 kHz (default) | One-sided cutoff, adjustable 50–225 kHz |
| FIR2 taps | 255 | Rejects FM stereo subcarrier (23–53 kHz) before D2 |
| FM max deviation | ±75 kHz | demodGain = ifSR / (2π × 75000) |
| De-emphasis | 50 µs (EU default) | First-order IIR, fc ≈ 3183 Hz |

### Why 500 kHz IF (not 250 kHz)

FIR1 must anti-alias before D1 decimation. Transition band = Nyquist − passband:
- 250 kHz IF: transition = 125 − 100 = 25 kHz → needs ~1000 taps (impractical)
- 500 kHz IF: transition = 250 − 150 = 100 kHz → 255 taps give ~-55 dB at Nyquist

### FIR1 push/compute optimization

FIR1 dot product is only computed at decimation output points (every D1-th sample). Between decimation points, samples are pushed into the delay line in O(1). This gives a D1× speedup (8× at 4 MS/s), critical for real-time processing.

## LimeSDR hardware configuration

Configuration matches **ExtIO_LimeSDR** (the HDSDR plugin) — known-good reference.

### Init sequence (LimeDevice::init)

```
LMS_Open → LMS_Init → LMS_SetSampleRate(SR, oversample=2)
→ LMS_EnableChannel(RX+TX) → LMS_SetAntenna(auto by freq)
→ LMS_SetLPFBW(SR) [TIA-protected] → LMS_SetLOFrequency
→ LMS_SetupStream
```

### Key hardware rules

- **Analog LPF BW = sample rate.** Updated on every `setSampleRate()` call.
- **TIA protection around LMS_SetLPFBW.** LMS_SetLPFBW internally corrupts G_TIA_RFE. Must set TIA=3 before call, restore desired value after (learned from ExtIO source).
- **RCC_CTL_PGA_RBB compensation.** Written after every `setGain()` call. Formula from LMS7002M datasheet: `rcc = (430 × 0.65^(PGA/10) − 110.35) / 20.4516 + 16`. Without this, PGA baseband filter response degrades.
- **Calibration BW = max(SR, 2.5 MHz).** Matches ExtIO behaviour.
- **Gain to 0 dB before calibration.** Prevents MCU error 5 (LNA loopback at high gain). Gain + TIA restored after.
- **Antenna auto-select:** LNAW for < 1.5 GHz, LNAH for > 1.5 GHz.
- **Pending frequency via atomic.** During streaming, `setFrequency()` stores value in `pendingFrequency_` (atomic double). `readBlock()` applies it on the worker thread to avoid USB mutex contention.

### Gain structure (LimeSDR)

Single `setGain(dB)` API uses `LMS_SetGaindB` which distributes across LNA + PGA.
TIA is managed separately (fixed at `kDefaultTia = 3`, max gain).

| Stage | Range | Register |
|-------|-------|----------|
| LNA | 0–30 dB | G_LNA_RFE (1–15) |
| TIA | 0, 3, 12 dB | G_TIA_RFE (1–3), fixed at 3 |
| PGA | 0–31 dB | G_PGA_RBB (0–31) + RCC_CTL_PGA_RBB |

### Supported sample rates

`{2.5, 4, 5, 8, 10, 15, 20}` MS/s. All yield integer D1 for 500 kHz IF.
Minimum: 2.5 MS/s. Max recommended in Debug: 10 MS/s.

## Logging

Thread-safe singleton `Logger`. Output goes to `stand.log` next to the executable.
In Debug builds also mirrors to stderr. In Release — file only.

```cpp
LOG_INFO("message");
LOG_WARN("message");
LOG_ERROR("message");
LOG_DEBUG("message");
```

Format: `[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message`

## UI structure

### DeviceSelectionWindow
Lists detected LimeSDR devices. Click to open DeviceDetailWindow.

### DeviceDetailWindow (3 pages)

**Device Info** — serial, name, current sample rate.

**Device Control** — init, calibrate, sample rate selector, gain slider (0–68 dB).

**FFT / Radio** — spectrum plot + recording + FM controls:
- Spectrum: dark theme, center line (red), filter band overlay (green)
- Click spectrum to tune VFO, scroll to zoom X-axis, double-click to reset
- Raw .raw recording toggle
- Bandpass WAV export (offset, bandwidth, path)
- FM Radio: bandwidth (50–250 kHz), de-emphasis (50/75 µs), volume
- VFO tuner: absolute station frequency within capture band
- Signal level bar: SNR indicator (NOISE/MARGINAL/SIGNAL)
