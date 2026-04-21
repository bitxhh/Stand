#include "TxWorker.h"
#include "IDevice.h"
#include "ITxSource.h"
#include "Logger.h"

TxWorker::TxWorker(IDevice* device, ITxSource* source,
                   ChannelDescriptor channel, QObject* parent)
    : QObject(parent)
    , device_(device)
    , source_(source)
    , channel_(channel)
{
    buffer_.resize(kBlockSize * 2);  // interleaved I/Q
}

void TxWorker::stop() {
    running_.store(false);
}

void TxWorker::run() {
    const QString devId = device_->id();
    LOG_CAT(LogCat::kStreamIo, LogLevel::Info, "TxWorker started: ch"
             + std::to_string(channel_.channelIndex) + " on " + devId.toStdString());
    emit statusMessage(QString("TX active: %1").arg(devId));

    try {
        device_->startStream(channel_);
    } catch (const std::exception& ex) {
        const QString err = QString("TX startStream failed: %1").arg(ex.what());
        LOG_ERROR(err.toStdString());
        emit errorOccurred(err);
        emit finished();
        return;
    }

    source_->onTxStarted(device_->sampleRate());
    running_.store(true);

    while (running_.load()) {
        const int n = source_->generateBlock(buffer_.data(), kBlockSize,
                                             device_->sampleRate());
        if (n == 0) break;
        if (n < 0) {
            LOG_ERROR("TxWorker: source generateBlock error");
            emit errorOccurred("TX source error");
            break;
        }

        const int sent = device_->writeBlock(channel_, buffer_.data(), n, 100);
        if (sent < 0) {
            LOG_ERROR("TxWorker: writeBlock error on " + devId.toStdString());
            emit errorOccurred("TX writeBlock error");
            break;
        }
    }

    source_->onTxStopped();
    device_->stopStream(channel_);

    LOG_CAT(LogCat::kStreamIo, LogLevel::Info, "TxWorker finished: ch"
             + std::to_string(channel_.channelIndex) + " on " + devId.toStdString());
    emit statusMessage(QString("TX stopped: %1").arg(devId));
    emit finished();
}
