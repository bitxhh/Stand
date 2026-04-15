#pragma once

#include "../Core/IPipelineHandler.h"

#include <QByteArray>
#include <QObject>
#include <atomic>
#include <chrono>

// ---------------------------------------------------------------------------
// ClassifierHandler — IPipelineHandler that serializes every ~100 ms I/Q block
// into a binary frame and emits frameReady() for ClassifierController to send
// to the Python classifier service.
//
// Threading: processBlock() is called on the RxWorker thread.
//            frameReady() must be connected via Qt::QueuedConnection so the
//            ClassifierController can forward data to the QTcpSocket on the
//            main thread safely.
//
// Frame layout (little-endian):
//   [4B uint32  payload length (everything after these 4 bytes)]
//   [8B uint64  hardware timestamp]
//   [4B int32   sample count N]
//   [8B float64 sample rate Hz]
//   [N*2*4B float32 I/Q pairs, interleaved: I0,Q0,I1,Q1,...]
// ---------------------------------------------------------------------------
class ClassifierHandler : public QObject, public IPipelineHandler {
    Q_OBJECT

public:
    explicit ClassifierHandler(QObject* parent = nullptr);

    // IPipelineHandler
    void processBlock(const float* iq, int count, double sampleRateHz) override;
    void processBlock(const float* iq, int count, double sampleRateHz,
                      const BlockMeta& meta) override;

    // Minimum milliseconds between frames sent to classifier (rate limit).
    void setIntervalMs(int ms);   // default 100 ms; thread-safe

signals:
    // Emitted on RxWorker thread — connect via Qt::QueuedConnection.
    void frameReady(QByteArray frame);

private:
    static QByteArray serialize(const float* iq, int count,
                                double sampleRateHz, uint64_t timestamp);

    std::atomic<int> intervalMs_{100};

    using Clock = std::chrono::steady_clock;
    Clock::time_point lastEmit_{};
};
