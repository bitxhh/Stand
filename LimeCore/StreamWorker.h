#pragma once

#include <QObject>
#include <QVector>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include "Device.h"
#include "BandpassExporter.h"

// StreamWorker живёт в отдельном QThread.
//
// Поток данных:
//   QThread → StreamWorker::run() → LMS_RecvStream
//     ├─ (каждый блок)  → запись в файл (если включена)
//     └─ (≤ kPlotFps раз/с) → emit samplesReady() → UI рисует FFT
//
// Throttle:
//   samplesReady эмитится не чаще kPlotFps (30) раз в секунду.
//   Промежуточные блоки молча выбрасываются — для живого спектра они не нужны.
//   Файл при этом пишется для КАЖДОГО блока — без потерь.
//
// Остановка:
//   UI вызывает stop() → isRunning = false → run() завершается → emit finished()

class StreamWorker : public QObject {
    Q_OBJECT

public:
    explicit StreamWorker(std::shared_ptr<Device> device, QObject* parent = nullptr);

    void enableFileOutput(const QString& path);
    void disableFileOutput();

    // Включить фильтрацию и запись WAV.
    // stationOffsetHz — смещение нужной станции от центральной частоты LO (Гц).
    //   Пример: LO=102 MHz, станция=104 MHz → offset=+2e6
    //   Если станция = LO → offset=0.
    // inputSampleRateHz берётся в момент вызова run(), не здесь.
    void enableBandpassWav(const QString& wavPath,
                           double stationOffsetHz,
                           double bandwidthHz        = 100'000.0,
                           double outputSampleRateHz = 250'000.0);
    void disableBandpassWav();

    // Целевой FPS для emit samplesReady (не влияет на запись в файл).
    // Можно поменять до вызова run().
    void setPlotFps(int fps);

public slots:
    void run();   // подключить к QThread::started
    void stop();  // потокобезопасно

signals:
    // Блок I/Q сэмплов — чередующиеся I,Q,I,Q... в int16.
    // Гарантированно эмитится не чаще setPlotFps() раз в секунду.
    void samplesReady(QVector<int16_t> samples);

    void statusMessage(const QString& message);
    void finished();
    void errorOccurred(const QString& error);

private:
    std::shared_ptr<Device> device_;
    std::atomic<bool>       isRunning_{false};

    bool    fileOutputEnabled_{false};
    QString filePath_;

    // Bandpass WAV export (optional)
    bool    bandpassEnabled_{false};
    QString bandpassWavPath_;
    double  bandpassOffsetHz_{0.0};
    double  bandpassBwHz_{100'000.0};
    double  bandpassOutSrHz_{250'000.0};

    // Throttle: emit samplesReady не чаще этого интервала
    std::chrono::milliseconds plotInterval_{1000 / 30};   // 30 fps default

    // Buffer lives on the heap so size can be adjusted without touching the stack.
    // 16384 = 2^14 — FFTW radix-2 fast path, ~122 Hz/bin at 2 MHz SR.
    // Resized in run() to match the actual device sample rate if needed.
    static constexpr int    kSampleCnt = 16384;
    std::vector<int16_t>    buffer_;
};
