#pragma once

#include "../Core/ChannelDescriptor.h"
#include <QObject>
#include <atomic>
#include <cstdint>
#include <vector>

class IDevice;
class Pipeline;

// ---------------------------------------------------------------------------
// StreamWorker — чистый I/Q loop. Живёт в отдельном QThread.
//
// Единственная обязанность: читать блоки с устройства и передавать
// их в Pipeline. Вся обработка сигнала — в IPipelineHandler реализациях.
//
// Поток управления:
//   QThread::started → run() → IDevice::startStream()
//                    → loop: IDevice::readBlock() → Pipeline::dispatchBlock()
//                    → IDevice::stopStream() → finished()
// ---------------------------------------------------------------------------
class StreamWorker : public QObject {
    Q_OBJECT

public:
    // channel defaults to {RX, 0} — single-channel callers (AppController) unchanged.
    StreamWorker(IDevice* device, Pipeline* pipeline,
                 ChannelDescriptor channel = {},
                 QObject* parent = nullptr);

public slots:
    void run();
    void stop();

signals:
    void started();
    void finished();
    void errorOccurred(const QString& error);
    void statusMessage(const QString& msg);

private:
    IDevice*          device_;
    Pipeline*         pipeline_;
    ChannelDescriptor channel_{};
    std::atomic<bool> running_{false};

    // 16384 = 2^14: FFTW fast radix-2, помещается в USB transfer limit при любом SR.
    static constexpr int kBlockSize = 16384;
    std::vector<int16_t> buffer_;
};
