#include "StreamWorker.h"
#include "LimeException.h"
#include "Logger.h"

#include <QVector>
#include <cmath>
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

void StreamWorker::enableBandpassWav(const QString& wavPath,
                                     double stationOffsetHz,
                                     double bandwidthHz,
                                     double outputSampleRateHz) {
    bandpassEnabled_  = true;
    bandpassWavPath_  = wavPath;
    bandpassOffsetHz_ = stationOffsetHz;
    bandpassBwHz_     = bandwidthHz;
    bandpassOutSrHz_  = outputSampleRateHz;
}

void StreamWorker::disableBandpassWav() {
    bandpassEnabled_ = false;
}

void StreamWorker::setPlotFps(int fps) {
    if (fps > 0)
        plotInterval_ = std::chrono::milliseconds(1000 / fps);
}

void StreamWorker::stop() {
    isRunning_.store(false);
}

void StreamWorker::run() {
    using Clock = std::chrono::steady_clock;

    const std::string serial = device_->GetSerial();
    LOG_INFO("StreamWorker started for: " + serial);
    emit statusMessage(QString("Streaming: %1").arg(QString::fromStdString(serial)));

    // ── Открываем файл если нужно ────────────────────────────────────────────
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

    // ── Открываем WAV-экспортёр если нужно ───────────────────────────────────
    std::unique_ptr<BandpassExporter> bpExp;
    if (bandpassEnabled_) {
        const double inputSR = device_->get_sample_rate();
        try {
            bpExp = std::make_unique<BandpassExporter>(
                inputSR, bandpassOffsetHz_, bandpassBwHz_, bandpassOutSrHz_);
            if (!bpExp->open(bandpassWavPath_)) {
                emit errorOccurred(QString("Cannot open WAV: %1").arg(bandpassWavPath_));
                bpExp.reset();
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(std::string("BandpassExporter init failed: ") + ex.what());
            emit errorOccurred(QString("Bandpass init failed: %1").arg(ex.what()));
            bpExp.reset();
        }
    }

    // ── Выделяем буфер под I/Q (2 значения int16 на сэмпл) ─────────────────
    // Размер — степень двойки для быстрого пути FFTW.
    // Пересчитываем под реальный SR: при SR ≤ 2 MHz → 16384, иначе пропорционально.
    {
        const double sr    = device_->get_sample_rate();
        const int    ratio = static_cast<int>(std::ceil(sr / 2'000'000.0));
        const int    sz    = kSampleCnt * std::max(1, ratio);
        buffer_.assign(sz * 2, 0);   // *2: I и Q чередуются
        LOG_INFO("StreamWorker buffer: " + std::to_string(sz)
                 + " samples (" + std::to_string(sz * 2 * 2) + " bytes) at "
                 + std::to_string(static_cast<int>(sr)) + " Hz SR");
    }
    const int bufSamples = static_cast<int>(buffer_.size() / 2);

    // ── Обновляем стрим если SR изменился после init_device ─────────────────
    // init_device настраивает стрим на 30 MHz. Если пользователь сменил SR,
    // нужно пересоздать стрим под новый SR — иначе USB-пакеты не совпадают.
    {
        const double currentSR = device_->get_sample_rate();
        const double initSR    = 30'000'000.0;
        if (std::abs(currentSR - initSR) > 1.0) {
            LOG_INFO("SR changed from init (" + std::to_string(static_cast<int>(initSR))
                     + " → " + std::to_string(static_cast<int>(currentSR))
                     + " Hz), refreshing stream setup");
            try {
                device_->teardown_stream();
                device_->setup_stream();
            } catch (const std::exception& ex) {
                const QString err = QString("Stream refresh failed: %1").arg(ex.what());
                LOG_ERROR(err.toStdString());
                emit errorOccurred(err);
                emit finished();
                return;
            }
        }
    }

    // ── Стартуем стрим на железе ─────────────────────────────────────────────
    if (LMS_StartStream(&device_->streamId) != 0) {
        const QString err = "LMS_StartStream failed";
        LOG_ERROR(err.toStdString());
        emit errorOccurred(err);
        device_->teardown_stream();
        emit finished();
        return;
    }
    LOG_INFO("LMS_StartStream OK for: " + serial);

    isRunning_.store(true);

    auto lastPlotTime  = Clock::now() - plotInterval_;
    int  diagCount     = 0;
    int  lastGoodRead  = 0;   // track expected block size for dropout detection

    // ── Основной цикл ─────────────────────────────────────────────────────────
    while (isRunning_.load()) {
        const int samplesRead = LMS_RecvStream(
            &device_->streamId, buffer_.data(), bufSamples, nullptr, 1000);

        // Diagnostic: log first 10 calls
        if (diagCount < 10) {
            LOG_DEBUG("LMS_RecvStream[" + std::to_string(diagCount)
                      + "] = " + std::to_string(samplesRead));
            ++diagCount;
        }

        if (samplesRead < 0) {
            LOG_ERROR("LMS_RecvStream error for " + serial);
            emit errorOccurred("LMS_RecvStream returned error");
            break;
        }

        if (samplesRead == 0)
            continue;

        // ── USB dropout detection ─────────────────────────────────────────────
        // A sudden drop from full buffer to a partial read means WinUSB lost a
        // transfer packet. The device is now in an unknown state — continuing
        // would produce corrupt data and cause subsequent LimeSuite calls to fail.
        // Stop cleanly and ask the user to restart the stream.
        if (lastGoodRead == bufSamples && samplesRead < bufSamples / 2) {
            LOG_WARN("USB dropout detected: expected " + std::to_string(bufSamples)
                     + " samples, got " + std::to_string(samplesRead)
                     + ". Stopping stream.");
            emit errorOccurred(
                QString("USB transfer error — stream stopped.\n"
                        "Unplug and replug the device if this recurs."));
            break;
        }
        lastGoodRead = samplesRead;

        // ── Запись в файл (каждый блок, без потерь) ──────────────────────────
        if (fileOutputEnabled_ && outFile.is_open()) {
            outFile.write(reinterpret_cast<const char*>(buffer_.data()),
                          static_cast<std::streamsize>(samplesRead) * 2 * sizeof(int16_t));
        }

        // ── Bandpass + WAV export (каждый блок) ──────────────────────────────
        if (bpExp) {
            QVector<int16_t> block(buffer_.data(), buffer_.data() + samplesRead * 2);
            bpExp->pushBlock(block);
        }

        // ── Throttle: emit только если прошло достаточно времени ─────────────
        const auto now = Clock::now();
        if (now - lastPlotTime >= plotInterval_) {
            lastPlotTime = now;

            QVector<int16_t> block(buffer_.data(), buffer_.data() + samplesRead * 2);
            emit samplesReady(std::move(block));

            LOG_DEBUG("StreamWorker plot frame: "
                      + std::to_string(samplesRead) + " samples from " + serial);
        }
    }

    // ── Завершение ────────────────────────────────────────────────────────────
    // teardown_stream() вызывает LMS_StopStream + LMS_DestroyStream внутри —
    // не дублируем явный StopStream здесь.
    device_->teardown_stream();

    if (outFile.is_open()) {
        outFile.flush();
        outFile.close();
    }

    if (bpExp) {
        bpExp->close();
        bpExp.reset();
    }

    LOG_INFO("StreamWorker finished for: " + serial);
    emit statusMessage(QString("Stream stopped: %1").arg(QString::fromStdString(serial)));
    emit finished();
}
