#pragma once

#include "../Core/ChannelDescriptor.h"
#include <QObject>
#include <atomic>
#include <cstdint>
#include <vector>

class IDevice;
class ITxSource;

// ---------------------------------------------------------------------------
// TxWorker — чистый TX loop. Живёт в отдельном QThread.
//
// Поток управления:
//   QThread::started → run() → IDevice::startStream(ch)
//                    → loop: ITxSource::generateBlock() → IDevice::writeBlock(ch)
//                    → IDevice::stopStream(ch) → finished()
// ---------------------------------------------------------------------------
class TxWorker : public QObject {
    Q_OBJECT

public:
    TxWorker(IDevice* device, ITxSource* source,
             ChannelDescriptor channel = {ChannelDescriptor::TX, 0},
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
    ITxSource*        source_;
    ChannelDescriptor channel_{ChannelDescriptor::TX, 0};
    std::atomic<bool> running_{false};

    static constexpr int kBlockSize = 16384;
    std::vector<int16_t> buffer_;
};
