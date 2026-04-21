#include "RxWorker.h"
#include "IDevice.h"
#include "Pipeline.h"
#include "IPipelineHandler.h"
#include "Logger.h"

RxWorker::RxWorker(IDevice* device, Pipeline* pipeline,
                           ChannelDescriptor channel, QObject* parent)
    : QObject(parent)
    , device_(device)
    , pipeline_(pipeline)
    , channel_(channel)
{
    buffer_.resize(kBlockSize * 2);     // interleaved I/Q: count * 2 int16
    floatBuf_.resize(kBlockSize * 2);   // normalized float32 copy
}

void RxWorker::stop() {
    running_.store(false);
}

void RxWorker::run() {
    const QString devId = device_->id();
    LOG_CAT(LogCat::kStreamIo, LogLevel::Info, "RxWorker started: " + devId.toStdString());
    emit statusMessage(QString("Streaming: %1").arg(devId));

    // Старт стрима
    try {
        device_->startStream(channel_);
    } catch (const std::exception& ex) {
        const QString err = QString("startStream failed: %1").arg(ex.what());
        LOG_ERROR(err.toStdString());
        emit errorOccurred(err);
        emit finished();
        return;
    }

    const double sr = device_->sampleRate();
    pipeline_->notifyStarted(sr);
    running_.store(true);

    int diagCount = 0;

    // Основной цикл
    while (running_.load()) {
        // Park point for UI-thread retune: if the main thread has set
        // retuneInProgress_, this blocks until LMS_StopStream/SetLO/StartStream
        // and handler state reset are done.  Must come BEFORE readBlock so
        // the worker isn't inside LMS_RecvStream when the UI mutates streams_.
        device_->checkPauseForRetune(channel_);

        // 100 ms timeout — keeps LMS_RecvStream from holding the device mutex
        // too long and blocking main-thread calls (e.g. LMS_SetLOFrequency).
        const int n = device_->readBlock(channel_, buffer_.data(), kBlockSize, 100);

        if (diagCount < 10) {
            LOG_CAT(LogCat::kStreamIo, LogLevel::Debug, "readBlock[" + std::to_string(diagCount) + "] = " + std::to_string(n));
            ++diagCount;
        }

        if (n < 0) {
            LOG_ERROR("readBlock error: " + devId.toStdString());
            emit errorOccurred("readBlock returned error");
            break;
        }
        if (n == 0) continue;

        // Log partial reads but don't stop — LimeSuite sometimes delivers
        // a smaller block after a USB hiccup and recovers on its own.
        if (n < kBlockSize) {
            LOG_WARN("readBlock partial: expected " + std::to_string(kBlockSize)
                     + " got " + std::to_string(n) + " — continuing");
        }

        // Single int16→float conversion at hardware boundary (/ 32768.0f → [-1, 1])
        for (int i = 0; i < n * 2; ++i)
            floatBuf_[i] = buffer_[i] * (1.0f / 32768.0f);

        pipeline_->dispatchBlock(floatBuf_.data(), n, sr,
                                BlockMeta{channel_, device_->lastReadTimestamp(channel_)});
    }

    pipeline_->notifyStopped();
    // NOTE: device_->stopStream(channel_) is intentionally NOT called here.
    // Mutating streams_ from the worker thread races with UI-thread operations
    // that touch the same map (setFrequency on a streaming channel, close()).
    // RxController calls stopStream on the main thread after finished().

    LOG_CAT(LogCat::kStreamIo, LogLevel::Info, "RxWorker finished: " + devId.toStdString());
    emit statusMessage(QString("Stream stopped: %1").arg(devId));
    emit finished();
}
