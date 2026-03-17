#include "StreamWorker.h"
#include "LimeException.h"
#include "Logger.h"

#include <QVector>
#include <fstream>

StreamWorker::StreamWorker(std::shared_ptr<Device> device, QObject* parent)
    : QObject(parent)
    , device_(std::move(device))
{}

void StreamWorker::enableFileOutput(const QString& path) {
    fileOutputEnabled_ = true;
    filePath_          = path;
}

void StreamWorker::disableFileOutput() {
    fileOutputEnabled_ = false;
}

void StreamWorker::stop() {
    isRunning_.store(false);
}

void StreamWorker::run() {
    const std::string serial = device_->GetSerial();
    LOG_INFO("StreamWorker started for: " + serial);
    emit statusMessage(QString("Streaming: %1").arg(QString::fromStdString(serial)));

    // Открываем файл если нужно
    std::ofstream outFile;
    if (fileOutputEnabled_) {
        outFile.open(filePath_.toStdString(), std::ios::binary);
        if (!outFile.is_open()) {
            const QString err = QString("Cannot open file: %1").arg(filePath_);
            LOG_ERROR(err.toStdString());
            emit errorOccurred(err);
            emit finished();
            return;
        }
        LOG_INFO("File output enabled: " + filePath_.toStdString());
    }

    // Стартуем стрим на железе
    if (LMS_StartStream(&device_->streamId) != 0) {
        const QString err = "LMS_StartStream failed";
        LOG_ERROR(err.toStdString());
        emit errorOccurred(err);
        emit finished();
        return;
    }

    isRunning_.store(true);

    while (isRunning_.load()) {
        const int samplesRead = LMS_RecvStream(
            &device_->streamId, buffer_, kSampleCnt, nullptr, 1000);

        if (samplesRead > 0) {
            // Копируем в QVector и шлём в FFT-страницу через сигнал
            QVector<int16_t> block(buffer_, buffer_ + samplesRead * 2);
            emit samplesReady(std::move(block));

            // Пишем в файл если включено
            if (fileOutputEnabled_ && outFile.is_open()) {
                outFile.write(reinterpret_cast<const char*>(buffer_),
                              static_cast<std::streamsize>(samplesRead) * 2 * sizeof(int16_t));
            }

            LOG_DEBUG("StreamWorker: " + std::to_string(samplesRead) + " samples from " + serial);

        } else if (samplesRead < 0) {
            LOG_ERROR("LMS_RecvStream error for " + serial);
            emit errorOccurred("LMS_RecvStream returned error");
            break;
        }
    }

    LMS_StopStream(&device_->streamId);

    if (outFile.is_open()) {
        outFile.flush();
        outFile.close();
    }

    LOG_INFO("StreamWorker finished for: " + serial);
    emit statusMessage(QString("Stream stopped: %1").arg(QString::fromStdString(serial)));
    emit finished();
}
