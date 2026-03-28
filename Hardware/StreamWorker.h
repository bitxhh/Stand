#pragma once

#include <QObject>
#include <QVector>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include "Device.h"
#include "BandpassExporter.h"
#include "FmDemodulator.h"

// StreamWorker живёт в отдельном QThread.
//
// Поток данных:
//   QThread → StreamWorker::run() → LMS_RecvStream
//     ├─ (каждый блок)  → запись в файл (если включена)
//     ├─ (каждый блок)  → BandpassExporter → WAV (если включён)
//     ├─ (каждый блок)  → FmDemodulator → emit audioReady() (если включён)
//     └─ (≤ kPlotFps раз/с) → emit samplesReady() → UI рисует FFT
//
// Аудио:
//   audioReady(QVector<float>, double) — нормализованные моно-сэмплы float32
//   на частоте FmDemodulator::audioSampleRate() (~50 kHz).
//   Принимается в главном потоке, записывается в QAudioSink (push mode).

class StreamWorker : public QObject {
    Q_OBJECT

public:
    explicit StreamWorker(std::shared_ptr<Device> device, QObject* parent = nullptr);

    void enableFileOutput(const QString& path);
    void disableFileOutput();

    // Bandpass WAV export
    void enableBandpassWav(const QString& wavPath,
                           double stationOffsetHz,
                           double bandwidthHz        = 100'000.0,
                           double outputSampleRateHz = 250'000.0);
    void disableBandpassWav();

    // FM demodulator
    // stationOffsetHz — смещение станции от LO (Гц).
    // deemphTauSec    — 75e-6 (Европа), 50e-6 (США).
    // bandwidthHz     — ширина полосового фильтра (Гц), по умолч. 100 кГц (WBFM).
    void enableFmDemod(double stationOffsetHz,
                       double deemphTauSec  = 75e-6,
                       double bandwidthHz   = 100'000.0);
    void disableFmDemod();
    // Update FIR1 bandwidth on the fly while stream is running (thread-safe).
    void setFmBandwidth(double bandwidthHz);

    // Целевой FPS для emit samplesReady (не влияет на запись / демодуляцию).
    void setPlotFps(int fps);

public slots:
    void run();   // подключить к QThread::started
    void stop();  // потокобезопасно

signals:
    // I/Q блок для FFT-графика (эмитируется не чаще setPlotFps() раз/с)
    void samplesReady(QVector<int16_t> samples);

    // Демодулированное аудио (float32 моно, нормализованное ±1).
    // sampleRateHz — частота дискретизации (обычно ~50 000 Гц).
    // Управление громкостью — через QAudioSink::setVolume() на стороне UI.
    void audioReady(QVector<float> samples, double sampleRateHz);

    void statusMessage(const QString& message);
    void finished();
    void errorOccurred(const QString& error);

private:
    std::shared_ptr<Device> device_;
    std::atomic<bool>       isRunning_{false};

    // Raw file output
    bool    fileOutputEnabled_{false};
    QString filePath_;

    // Bandpass WAV export
    bool    bandpassEnabled_{false};
    QString bandpassWavPath_;
    double  bandpassOffsetHz_{0.0};
    double  bandpassBwHz_{100'000.0};
    double  bandpassOutSrHz_{250'000.0};

    // FM demodulator
    bool    fmEnabled_{false};
    double  fmOffsetHz_{0.0};
    double  fmDeemphTau_{75e-6};
    double  fmBandwidthHz_{100'000.0};
    // Pending bandwidth update — written from UI thread, read in run() loop.
    // std::atomic<double> is not guaranteed lock-free on all platforms; we use
    // a simple flag+value pair protected by the fact that only one write and
    // one read happen per audio block (~8 ms interval at 2 MHz SR).
    std::atomic<double> fmBandwidthPending_{0.0};   // 0 = no update pending

    // Throttle: emit samplesReady не чаще этого интервала
    std::chrono::milliseconds plotInterval_{1000 / 30};

    // I/Q buffer (resized in run() based on actual SR)
    static constexpr int  kSampleCnt = 16384;
    std::vector<int16_t>  buffer_;
};
