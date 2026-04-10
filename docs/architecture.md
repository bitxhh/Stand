# Architecture

## Component diagram

```
main.cpp
  ├── LimeDeviceManager          — scans USB for LimeSDR devices
  └── Application                — QApplication + DeviceSelectionWindow + SessionManager
        └── DeviceDetailWindow   — main UI window per device (4-page navigation)
              ├── DeviceController     — thin command layer (UI → IDevice), no widgets
              ├── QThreadPool (dspPool_) — shared across all RX-channel pipelines
              │
              ├── [per RX channel] ChannelPanel
              │     └── AppController
              │           ├── StreamWorker (QThread)   — blocking I/Q recv loop
              │           │     └── Pipeline            — dispatches I/Q blocks to handlers
              │           │           ├── FftHandler          → spectrum display
              │           │           ├── [Fm|Am]DemodHandler → audio demodulator
              │           │           ├── RawFileHandler      → .raw I/Q dump
              │           │           ├── BandpassHandler     → filtered WAV export
              │           │           └── ClassifierHandler   → AI signal classifier (optional)
              │           └── FmAudioOutput    — resampler + AGC + QAudioSink (WASAPI)
              │
              └── TxController
                    └── TxWorker (QThread)   — blocking I/Q transmit loop
                          └── ITxSource (ToneSource / ...) — I/Q generator
```

## Directory layout

```
Core/               Interfaces and infrastructure
  IDevice.h           SDR-agnostic device interface (channel-aware: RX0/RX1/TX0/TX1)
  IDeviceManager.h    Device discovery interface
  IPipelineHandler.h  Signal processing handler interface + BlockMeta
  Pipeline.h/.cpp     I/Q block router (shared_mutex + optional parallel dispatch)
  ChannelDescriptor.h {Direction RX|TX, int channelIndex}
  ISyncController.h   3-level sync interface: clock / timestamp / trigger (Phase 6)
  Logger.h/.cpp       Thread-safe singleton logger
  LimeException.h     Exception hierarchy for LimeSuite errors

Hardware/           LimeSDR implementation
  LimeDevice.h/.cpp          IDevice for LimeSDR (LimeSuite C API, dual RX/TX)
  LimeDeviceManager.h/.cpp   IDeviceManager, USB device scanning
  LimeSyncController.h/.cpp  ISyncController stub (Phase 6)
  DeviceController.h/.cpp    Exception-safe UI→device command wrapper
  StreamWorker.h/.cpp        QThread I/Q recv loop, pipeline dispatch (channel-aware)
  TxWorker.h/.cpp            QThread I/Q transmit loop

DSP/                Signal processing
  FftProcessor.h/.cpp        Stateless FFT (FFTW3, thread-local plan cache)
  FftHandler.h/.cpp          IPipelineHandler: throttled FFT + EMA smoothing
  FmDemodulator.h/.cpp       Stateful WBFM demodulator (full DSP chain)
  FmDemodHandler.h/.cpp      IPipelineHandler wrapper for FmDemodulator
  AmDemodulator.h/.cpp       Stateful AM envelope demodulator
  AmDemodHandler.h/.cpp      IPipelineHandler wrapper for AmDemodulator
  BaseDemodHandler.h/.cpp    Common base: SNR/RMS metrics, param dispatch
  DemodRegistry.h/.cpp       Factory registry: mode name → BaseDemodHandler*
  BandpassExporter.h/.cpp    NCO + FIR + decimate → WAV writer
  BandpassHandler.h/.cpp     IPipelineHandler wrapper for BandpassExporter
  RawFileHandler.h/.cpp      IPipelineHandler: raw int16 I/Q dump

Audio/              Audio output
  FmAudioOutput.h/.cpp       Linear resampler + AGC + QAudioSink

Application/        UI (Qt widgets only — no DSP, no hardware calls)
  Application.h/.cpp         DeviceSelectionWindow + DeviceDetailWindow
  AppController.h/.cpp       Owns Pipeline, StreamWorker, handlers, audio per channel
  ChannelPanel.h/.cpp        Self-contained per-channel widget (FFT plot, demod, recording)
  TxController.h/.cpp        Owns TxWorker + ITxSource
  ClassifierController.h/.cpp Python subprocess + TCP socket → ClassifierHandler in pipeline
  SessionManager.h/.cpp      Tracks which device IDs have open windows
  Logger.h/.cpp              UI-side log sink (QPlainTextEdit)
```

## Threading model

| Thread | Components | Responsibilities |
|--------|-----------|-----------------|
| **Main (Qt event loop)** | All widgets, DeviceController, FmAudioOutput, TxController | UI updates, audio sink writes, device commands, prepareStream (LimeSuite quirk) |
| **StreamWorker (QThread)** — one per RX channel | StreamWorker, Pipeline dispatch | Blocking `readBlock()`, pipeline dispatch, blocks until all handlers finish |
| **QThreadPool (dspPool_)** | IPipelineHandler tasks | Parallel handler execution within one Pipeline dispatch (FFT, demod, recording run concurrently per block) |
| **TxWorker (QThread)** | TxWorker, ITxSource | `generateBlock()` + `writeBlock()` loop |

Cross-thread signals: `Qt::QueuedConnection`. No shared mutable state between handlers (each handler is exclusive to one pipeline).

**LimeSuite prepareStream quirk:** `LMS_SetupStream` must be called from the UI thread before workers start — it stops all active streams. `DeviceDetailWindow::startStream()` calls `device->prepareStream(ch)` for all RX channels before launching workers. Workers call only `LMS_StartStream` / `LMS_StopStream`.

## Key interfaces

**IDevice** — SDR-agnostic device abstraction (channel-aware):
- `init()` / `calibrate()` / `setSampleRate()` / `setFrequency(ch)` / `setGain(ch)`
- `prepareStream(ch)` — UI thread only (LMS_SetupStream)
- `startStream(ch)` / `readBlock(ch, ...)` / `stopStream(ch)` — worker thread
- `writeBlock(ch, ...)` — TxWorker thread
- `lastReadTimestamp(ch)` — hardware sample counter for sync
- State machine: `Disconnected → Connected → Ready → Streaming → Error`

**IPipelineHandler** — signal processing stage:
```cpp
virtual void processBlock(const int16_t* iq, int count, double sampleRateHz) = 0;
virtual void processBlock(const int16_t* iq, int count, double sampleRateHz,
                          const BlockMeta& meta);   // default: delegates above
virtual void onStreamStarted(double sampleRateHz) {}
virtual void onStreamStopped() {}
```
When `Pipeline` has a `QThreadPool*`, each handler is dispatched as a separate pool task and called concurrently. Handlers must not share mutable state with each other.

**Pipeline** — reader-writer lock router with optional parallel dispatch:
- `addHandler()` / `removeHandler()` — exclusive lock
- `dispatchBlock()` — shared lock; parallel if `pool != nullptr && handlers > 1`
- Parallel path: `QtConcurrent::run(pool, lambda)` per handler + `waitForFinished()` barrier (backpressure preserved)
- Sequential fallback: `pool == nullptr` or single handler (TX pipeline, tests)

**BlockMeta** — per-block metadata:
```cpp
struct BlockMeta {
    ChannelDescriptor channel{};    // which RX channel produced this block
    uint64_t          timestamp{0}; // hardware sample counter (0 = unavailable)
};
```

**AppController** — per-channel controller (no widgets):
- Owns `Pipeline`, `StreamWorker`, all handlers, `FmAudioOutput`
- `startStream(StreamConfig)` — creates pipeline + handlers + launches worker thread
- `stopStream()` — signals worker; cleanup on `streamFinished`
- `addExtraHandler()` / `removeExtraHandler()` — used by `ClassifierController` at runtime

**TxController** — TX lifecycle:
- `startTx(TxConfig)` — sets device frequency/gain, launches `TxWorker`
- `stopTx()` — signals worker to stop
- `TxConfig`: freqMHz, gainDb, sourceType, toneOffsetHz, amplitude

## Data flow

### I/Q → Spectrum
```
LimeDevice::readBlock(ch) → StreamWorker → Pipeline → [pool task] FftHandler
  int16[16384×2]                                                       ↓
                                                             Throttle 30 fps
                                                             FftProcessor::process()
                                                              ├─ Hann window
                                                              ├─ FFTW forward
                                                              ├─ FFT-shift (DC center)
                                                              ├─ Power → dBFS
                                                              └─ EMA smooth (α=0.1)
                                                                    ↓ emit fftReady()
                                                             ChannelPanel::onFftReady()
                                                             QCustomPlot::replot() (plotTimer 50ms)
```

### I/Q → Audio (FM or AM) — parallel with FFT
```
Pipeline → [pool task] [Fm|Am]DemodHandler → [Fm|Am]Demodulator
                                                      ↓
                                          QVector<float> @ 50 kHz
                                                      ↓ emit audioReady()
                                          FmAudioOutput::push()
                                            ├─ Linear resample → 48 kHz
                                            ├─ AGC (target 0.12 RMS)
                                            ├─ Mono → stereo
                                            └─ QAudioSink (WASAPI)
```

### TX path
```
TxController::startTx()
  ├─ device->setFrequency(TX0, freqHz)
  ├─ device->setGain(TX0, gainDb)
  └─ TxWorker::run():
       ITxSource::generateBlock() → int16[N×2]
         └─ IDevice::writeBlock(TX0, buffer, N, timeoutMs)
```

## UI structure

**DeviceSelectionWindow** — lists detected LimeSDR devices; one button per device. Uses `SessionManager` to prevent opening the same device twice.

**DeviceDetailWindow (4 pages via QListWidget + QStackedWidget):**

| Page | Content |
|------|---------|
| *Device Info* | Serial, name, current sample rate |
| *Device Control* | Init, calibrate, sample rate selector, gain slider (0–68 dB) |
| *FFT* | One `ChannelPanel` per RX channel in a scroll area + Start/Stop stream buttons |
| *Transmit* | TX frequency, TX gain, tone offset + amplitude, Start/Stop TX buttons |

**ChannelPanel** (one per RX channel, lives in the FFT page):
- Dark theme FFT spectrum plot (QCustomPlot), center line, VFO filter band overlay
- Click to tune, scroll to zoom X, double-click to reset zoom
- Frequency spin + slider, gain slider
- Mode selector: Off / FM / AM (hot-swap mid-stream)
- FM: bandwidth (50–250 kHz), de-emphasis (50/75 µs), volume
- AM: bandwidth (1–20 kHz), volume
- Raw .raw recording toggle + filtered WAV export (path + offset + BW)
- Classifier toggle (starts/stops Python AI subprocess)
- SNR bar: NOISE / MARGINAL / SIGNAL

## Planned: 3-tab restructure (RX / TX / RX+TX)

The current single-window layout will be replaced with three top-level tabs:
- **RX** — per-channel panels (current FFT page contents)
- **TX** — transmit controls (current Transmit page)
- **RX+TX** — coordinated full-duplex mode with ISyncController integration

This separation keeps RX, TX, and duplex logic in distinct code paths.
