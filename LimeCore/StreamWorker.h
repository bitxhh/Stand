#pragma once

#include <QObject>
#include <QVector>
#include <atomic>
#include <memory>
#include "Device.h"

// StreamWorker живёт в отдельном QThread.
// UI создаёт его, подключает сигналы, двигает в поток и вызывает start().
//
// Поток данных:
//   QThread → StreamWorker::run() → LMS_RecvStream → emit samplesReady()
//                                                  → FFT-страница рисует график
//
// Остановка:
//   UI вызывает StreamWorker::stop() → isRunning = false → run() завершается
//   → emit finished() → QThread::quit()

class StreamWorker : public QObject {
    Q_OBJECT

public:
    explicit StreamWorker(std::shared_ptr<Device> device, QObject* parent = nullptr);

    // Включить/выключить запись в файл до вызова run()
    void enableFileOutput(const QString& path);
    void disableFileOutput();

public slots:
    void run();   // запускается через connect(thread, &QThread::started, worker, &StreamWorker::run)
    void stop();  // можно вызвать из любого потока

signals:
    // Блок I/Q сэмплов — чередующиеся I,Q,I,Q... в int16
    void samplesReady(QVector<int16_t> samples);

    void statusMessage(const QString& message);
    void finished();
    void errorOccurred(const QString& error);

private:
    std::shared_ptr<Device> device_;
    std::atomic<bool>       isRunning_{false};

    bool    fileOutputEnabled_{false};
    QString filePath_;

    static constexpr int kSampleCnt = 5000;
    int16_t buffer_[kSampleCnt * 2]{};
};
