# Architecture

## Component diagram

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
              │           ├── AmDemodHandler   → AM envelope demod → audio
              │           ├── RawFileHandler   → .raw I/Q dump
              │           └── BandpassHandler  → filtered WAV export
              └── FmAudioOutput        — resampler + AGC + QAudioSink (WASAPI)
```

## Directory layout

```
Core/               Interfaces and infrastructure
  IDevice.h           SDR-agnostic device interface
  IDeviceManager.h    Device discovery interface
  IPipelineHandler.h  Signal processing handler interface
  Pipeline.h/.cpp     I/Q block router (shared_mutex reader-writer lock)
  Logger.h/.cpp       Thread-safe singleton logger
  LimeException.h     Exception hierarchy for LimeSuite errors

Hardware/           LimeSDR implementation
  LimeDevice.h/.cpp          IDevice for LimeSDR (LimeSuite C API)
  LimeDeviceManager.h/.cpp   IDeviceManager, USB device scanning
  DeviceController.h/.cpp    Exception-safe UI→device command wrapper
  StreamWorker.h/.cpp        QThread I/Q loop, pipeline dispatch

DSP/                Signal processing
  FftProcessor.h/.cpp        Stateless FFT (FFTW3, thread-local plan cache)
  FftHandler.h/.cpp          IPipelineHandler: throttled FFT + EMA smoothing
  FmDemodulator.h/.cpp       Stateful WBFM demodulator (full DSP chain)
  FmDemodHandler.h/.cpp      IPipelineHandler wrapper for FmDemodulator
  AmDemodulator.h/.cpp       Stateful AM envelope demodulator (full DSP chain)
  AmDemodHandler.h/.cpp      IPipelineHandler wrapper for AmDemodulator
  BandpassExporter.h/.cpp    NCO + FIR + decimate → WAV writer
  BandpassHandler.h/.cpp     IPipelineHandler wrapper for BandpassExporter
  RawFileHandler.h/.cpp      IPipelineHandler: raw int16 I/Q dump

Audio/              Audio output
  FmAudioOutput.h/.cpp       Linear resampler + AGC + QAudioSink

Application/        UI (Qt widgets only — no DSP, no hardware calls)
  Application.h/.cpp         DeviceSelectionWindow + DeviceDetailWindow
```

## Threading model

| Thread | Components | Responsibilities |
|--------|-----------|-----------------|
| **Main (Qt event loop)** | All widgets, DeviceController, FmAudioOutput | UI updates, audio sink writes, device commands |
| **StreamWorker (QThread)** | StreamWorker, Pipeline, all IPipelineHandlers | I/Q recv, FFT, FM/AM demod, file I/O |

Cross-thread: `Qt::QueuedConnection`. No shared mutable state except atomics for pending parameter updates.

## Key interfaces

**IDevice** — SDR-agnostic device abstraction:
- `init()` / `calibrate()` / `setSampleRate()` / `setFrequency()` / `setGain()`
- `startStream()` / `readBlock()` / `stopStream()` (called from worker thread)
- State machine: `Disconnected → Connected → Ready → Streaming → Error`

**IPipelineHandler** — signal processing stage:
```cpp
virtual void processBlock(const int16_t* iq, int count, double sampleRateHz) = 0;
virtual void onStreamStarted(double sampleRateHz) {}
virtual void onStreamStopped() {}
```
Called synchronously in StreamWorker thread. Budget: ~4 ms at 4 MS/s.

**Pipeline** — reader-writer lock router:
- `addHandler()` / `removeHandler()` — exclusive lock, blocks until dispatch finishes
- `dispatchBlock()` / `notifyStarted()` / `notifyStopped()` — shared lock
- Prevents use-after-free when switching demod mode during active stream

## Data flow

### I/Q → Spectrum
```
LimeDevice::readBlock() → StreamWorker → Pipeline → FftHandler
  int16[16384×2]                                       ↓
                                               Throttle 30 fps
                                               FftProcessor::process()
                                                ├─ Hann window
                                                ├─ FFTW forward
                                                ├─ FFT-shift (DC center)
                                                ├─ Power → dBFS
                                                └─ EMA smooth (α=0.1)
                                                      ↓ emit fftReady()
                                               QCustomPlot::replot()
```

### I/Q → Audio (FM or AM)
```
LimeDevice::readBlock() → Pipeline → [Fm|Am]DemodHandler → [Fm|Am]Demodulator
                                                                    ↓
                                                        QVector<float> @ 50 kHz
                                                                    ↓ emit audioReady()
                                                        FmAudioOutput::push()
                                                          ├─ Linear resample → 48 kHz
                                                          ├─ AGC (target 0.12 RMS)
                                                          ├─ Mono → stereo
                                                          └─ QAudioSink (WASAPI)
```

## UI structure

**DeviceSelectionWindow** — lists detected LimeSDR devices.

**DeviceDetailWindow (3 pages):**
- *Device Info* — serial, name, current sample rate
- *Device Control* — init, calibrate, sample rate selector, gain slider (0–68 dB)
- *FFT / Radio* — spectrum plot + recording + demodulator controls:
  - Dark theme spectrum, center line (red), filter band overlay (green)
  - Click to tune VFO, scroll to zoom X, double-click to reset
  - Raw .raw recording toggle + filtered WAV export
  - Mode selector: Off / FM / AM (switches mid-stream without crash)
  - FM: bandwidth (50–250 kHz), de-emphasis (50/75 µs), volume
  - AM: bandwidth (1–20 kHz), volume
  - VFO tuner: absolute station frequency within capture band
  - Signal level bar: SNR indicator (NOISE/MARGINAL/SIGNAL)
