# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

The project uses CMake with MinGW and Qt6. Build from CLion using the `cmake-build-debug-mingw-qt` or `cmake-build-release-mingw-qt` profiles.

From the command line:
```bash
cmake -B cmake-build-debug-mingw-qt -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug-mingw-qt --target Stand
```

**Note:** Sample rates >= 15 MS/s are only reliable in Release builds — Debug builds cannot drain the USB FIFO fast enough and cause stream dropouts.

There are no automated tests.

## Dependencies

- **Qt 6.10** at `C:/Qt/6.10.0/mingw_64`
- **LimeSuite** at `C:/LimeSuite` (headers + `LimeSuite.dll`)
- **FFTW3** (double precision) at `external/FFTW/`
- **QCustomPlot** bundled at `external/qcustomplot/`

## Target Architecture (in progress)

The codebase is being refactored toward the following structure. New code should follow this design; existing code is being migrated incrementally.

### Layer diagram

```
main.cpp
  └── AppController                  — owns all devices and pipelines; no widgets
        └── QMap<id, DeviceContext>
              ├── IDevice            — SDR-agnostic interface (LimeDevice implements it)
              ├── Pipeline           — routes I/Q blocks to registered handlers
              │     ├── StreamWorker (QThread) — only recv loop, emits blockReady
              │     └── IPipelineHandler[]  — registered at runtime, called in worker thread:
              │           ├── FftHandler       -> emit fftReady(FftFrame)
              │           ├── FmDemodHandler   -> emit audioReady(QVector<float>)
              │           ├── RawFileHandler   -> writes .raw
              │           └── BandpassHandler  -> writes .wav
              └── DeviceDetailWindow — subscribes to AppController signals only
                    └── IDevice::createAdvancedWidget() — device-specific controls (LNA/TIA/PGA)
```

### Key architectural rules

- `Application/` — Qt widgets only. No DSP, no arithmetic, no direct hardware calls.
- `AppController` — creates, connects, and owns all non-UI objects. Sits between UI and hardware.
- `IDevice` / `IDeviceManager` — SDR-agnostic interfaces in `Core/`. LimeSDR implementation lives in `Hardware/Lime/`.
- `IPipelineHandler::processBlock()` is called synchronously in the StreamWorker thread — handlers must not block.
- Cross-thread communication uses Qt signals exclusively (QueuedConnection across threads).
- Multi-device: one `DeviceContext` (and one `QThread`) per open device. `AppController` manages all contexts.

### DeviceContext

```cpp
struct DeviceContext {
    std::shared_ptr<IDevice>  device;
    Pipeline*                 pipeline;
    QThread*                  thread;
    DeviceDetailWindow*       window;   // nullptr if closed
};
```

### IDevice interface (unified gain)

`setGain(double dB)` is the single gain entry point. Max gain and supported sample rates are queried via `maxGain()` and `supportedSampleRates()`. Device-specific controls (LNA/TIA/PGA) are exposed through `createAdvancedWidget()` — the UI inserts it if non-null, without knowing what it contains.

### Device state machine

`IDevice` emits `stateChanged(DeviceState)`. States: `Disconnected -> Connected -> Ready -> Streaming -> Error`. UI reacts to state changes; it never checks flags directly.

### IPipelineHandler interface

```cpp
class IPipelineHandler {
public:
    virtual void processBlock(const int16_t* iq, int count, double sampleRateHz) = 0;
    virtual void onStreamStarted(double sampleRateHz) {}
    virtual void onStreamStopped() {}
};
```

Handlers receive a raw pointer — no copy unless the handler explicitly needs one.

---

## Current (legacy) structure

The original structure being migrated away from. Still exists in the codebase during the transition.

```
main.cpp
  └── LimeManager           — device enumeration and lifecycle (RAII)
  └── Application           — QApplication + DeviceSelectionWindow entry point
        └── DeviceDetailWindow   — main UI: info / control / FFT pages
              ├── DeviceController   — translates UI values -> Device API calls; no widgets
              ├── StreamWorker       — lives in a dedicated QThread; owns the I/Q loop
              │     ├── BandpassExporter  — bandpass-filter + WAV export (optional)
              │     └── FmDemodulator     — WBFM demodulation chain (optional)
              └── FmAudioOutput      — receives float32 audio via QueuedConnection, writes to QAudioSink
```

### Threading model (current)

- **Main thread:** all Qt widgets, `DeviceController` slots, `FmAudioOutput::push()`
- **StreamWorker thread (`QThread`):** `StreamWorker::run()` calls `LMS_RecvStream`, runs `FmDemodulator`, writes files. Communicates with the main thread exclusively via Qt signals.
- `FftProcessor::process()` is called from the main thread on each `samplesReady` signal, before updating `QCustomPlot`.

### Data flow through StreamWorker (current)

Each I/Q block (16 384 samples, `int16_t` interleaved):
1. Written to `.raw` file — if enabled.
2. Passed to `BandpassExporter` -> WAV — if enabled.
3. Passed to `FmDemodulator::pushBlock()` -> `emit audioReady(QVector<float>, double)` — if enabled.
4. Throttled to `kPlotFps` (default 30 fps) -> `emit samplesReady(QVector<int16_t>)`.

### Device gain structure (current, LimeSDR-specific)

Three independent stages via `DeviceController::setGain(lna, tia, pga)`:
- **LNA:** indices 0-5 -> 0, 5, 10, 15, 20, 25.5 dB
- **TIA:** indices 0-2 -> 0, 9, 12 dB
- **PGA:** 0-31 -> 0-31 dB (1 dB/step). Max total ~68.5 dB.

### Stream setup sequence (current)

`Device::init_device()` -> `set_sample_rate()` -> `calibrate()` -> `setup_stream()` -> `LMS_StartStream` (inside `StreamWorker::run()`). `setup_stream()` must be called after the final `set_sample_rate()` so USB transfer sizes match current SR.

## FM demodulation DSP chain

`int16 I/Q -> DC blocker -> NCO freq-shift -> FIR1 LPF (complex) -> decimate D1 -> IF @ ~250 kHz -> FM discriminator -> de-emphasis IIR -> FIR2 LPF -> decimate D2=5 -> float32 audio @ ~50 kHz`

`D1 = round(inputSR / 250 000)`. For all supported sample rates the IF is always exactly 250 kHz and audio is always exactly 50 kHz.

## Logging

Singleton `Logger` (thread-safe). Use macros at call sites:
```cpp
LOG_INFO("message");
LOG_WARN("message");
LOG_ERROR("message");
LOG_DEBUG("message");
```
Output goes to `stand.log` next to the executable and is also emitted as a Qt signal for potential UI consumption.

## LimeManager stable sample rates

Minimum: 2.5 MS/s (hardware artefacts below this on LimeSDR Mini).
Maximum recommended in Debug: 10 MS/s.
All rates in `LimeManager::sampleRates` produce integer decimation ratios for the FM IF (250 kHz).
