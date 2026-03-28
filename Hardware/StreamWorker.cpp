#include "StreamWorker.h"
#include "IDevice.h"
#include "Pipeline.h"
#include "Logger.h"

StreamWorker::StreamWorker(IDevice* device, Pipeline* pipeline, QObject* parent)
    : QObject(parent)
    , device_(device)
    , pipeline_(pipeline)
{
    buffer_.resize(kBlockSize * 2);   // interleaved I/Q: count * 2 int16
}

void StreamWorker::stop() {
    running_.store(false);
}

void StreamWorker::run() {
    const QString devId = device_->id();
    LOG_INFO("StreamWorker started: " + devId.toStdString());
    emit statusMessage(QString("Streaming: %1").arg(devId));

    // Старт стрима
    try {
        device_->startStream();
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

    int lastGoodRead = 0;
    int diagCount    = 0;

    // Основной цикл
    while (running_.load()) {
        const int n = device_->readBlock(buffer_.data(), kBlockSize, 1000);

        if (diagCount < 10) {
            LOG_DEBUG("readBlock[" + std::to_string(diagCount) + "] = " + std::to_string(n));
            ++diagCount;
        }

        if (n < 0) {
            LOG_ERROR("readBlock error: " + devId.toStdString());
            emit errorOccurred("readBlock returned error");
            break;
        }
        if (n == 0) continue;

        // USB dropout detection
        if (lastGoodRead == kBlockSize && n < kBlockSize / 2) {
            LOG_WARN("USB dropout: expected " + std::to_string(kBlockSize)
                     + " got " + std::to_string(n));
            emit errorOccurred(
                "USB transfer error — stream stopped.\n"
                "Unplug and replug the device if this recurs.");
            break;
        }
        lastGoodRead = n;

        pipeline_->dispatchBlock(buffer_.data(), n, sr);
    }

    pipeline_->notifyStopped();
    device_->stopStream();

    LOG_INFO("StreamWorker finished: " + devId.toStdString());
    emit statusMessage(QString("Stream stopped: %1").arg(devId));
    emit finished();
}
