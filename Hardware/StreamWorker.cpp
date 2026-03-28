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

void StreamWorker::enableFmDemod(double stationOffsetHz,
                                 double deemphTauSec,
                                 double bandwidthHz) {
    fmEnabled_     = true;
    fmOffsetHz_    = stationOffsetHz;
    fmDeemphTau_   = deemphTauSec;
    fmBandwidthHz_ = bandwidthHz;
}

void StreamWorker::disableFmDemod() {
    fmEnabled_ = false;
}

void StreamWorker::setFmBandwidth(double bandwidthHz) {
    fmBandwidthPending_.store(bandwidthHz);
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

    // ── Raw file output ──────────────────────────────────────────────────────
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

    // ── BandpassExporter ─────────────────────────────────────────────────────
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

    // ── FmDemodulator ────────────────────────────────────────────────────────
    std::unique_ptr<FmDemodulator> fmDem;
    if (fmEnabled_) {
        const double inputSR = device_->get_sample_rate();
        // At SR > 10 MHz the sample-by-sample FIR (127 taps, complex) is too
        // expensive for unoptimised debug builds. Release builds (-O2) are fine
        // at any supported SR up to 30 MHz.
        if (inputSR > 10'000'000.0) {
            LOG_WARN("FmDemodulator: input SR " + std::to_string(static_cast<int>(inputSR))
                     + " Hz — CPU-heavy at debug optimisation level. "
                     "Consider using SR ≤ 10 MHz for debug, or build in Release.");
        }
        try {
            fmDem = std::make_unique<FmDemodulator>(
                inputSR, fmOffsetHz_, fmDeemphTau_, fmBandwidthHz_);
            LOG_INFO("FmDemodulator ready: D1=" + std::to_string(fmDem->decimation1())
                     + " IF=" + std::to_string(static_cast<int>(fmDem->ifSampleRate())) + " Hz"
                     + " audio=" + std::to_string(static_cast<int>(fmDem->audioSampleRate())) + " Hz");
        } catch (const std::exception& ex) {
            LOG_ERROR(std::string("FmDemodulator init failed: ") + ex.what());
            emit errorOccurred(QString("FM demod init failed: %1").arg(ex.what()));
            fmDem.reset();
        }
    }

    // ── I/Q buffer ───────────────────────────────────────────────────────────
    // Fixed at kSampleCnt regardless of SR.
    // Scaling the buffer with SR (×15 at 30 MHz) produces 245 760-sample
    // USB transfers that exceed the LimeSuite/WinUSB transfer-size limit and
    // cause LimeSuite to crash via exit(1) ("TransferPacket: Read failed").
    // kSampleCnt = 16384 (2^14) keeps FFTW on the fast radix-2 path and is
    // well within USB transfer limits at all supported sample rates.
    {
        const double sr = device_->get_sample_rate();
        buffer_.assign(kSampleCnt * 2, 0);   // *2: interleaved I and Q
        LOG_INFO("StreamWorker buffer: " + std::to_string(kSampleCnt)
                 + " samples (" + std::to_string(kSampleCnt * 2 * 2) + " bytes) at "
                 + std::to_string(static_cast<int>(sr)) + " Hz SR");
    }
    const int bufSamples = kSampleCnt;

    // ── Пересоздать стрим ────────────────────────────────────────────────────
    // teardown_stream() (предыдущего run() или init_device()) уничтожает
    // streamId через LMS_DestroyStream. Перед LMS_StartStream нужен
    // валидный streamId — всегда вызываем setup_stream() заново.
    // Это также подхватывает изменения SR сделанные между сессиями.
    try {
        if (device_->is_stream_ready())
            device_->teardown_stream();
        device_->setup_stream();
    } catch (const std::exception& ex) {
        const QString err = QString("Stream setup failed: %1").arg(ex.what());
        LOG_ERROR(err.toStdString());
        emit errorOccurred(err);
        emit finished();
        return;
    }

    // ── Старт стрима ─────────────────────────────────────────────────────────
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

    auto lastPlotTime = Clock::now() - plotInterval_;
    int  diagCount    = 0;
    int  lastGoodRead = 0;

    // ── Основной цикл ─────────────────────────────────────────────────────────
    while (isRunning_.load()) {
        const int samplesRead = LMS_RecvStream(
            &device_->streamId, buffer_.data(), bufSamples, nullptr, 1000);

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
        if (samplesRead == 0) continue;

        // ── USB dropout detection ─────────────────────────────────────────────
        if (lastGoodRead == bufSamples && samplesRead < bufSamples / 2) {
            LOG_WARN("USB dropout: expected " + std::to_string(bufSamples)
                     + " samples, got " + std::to_string(samplesRead));
            emit errorOccurred(
                QString("USB transfer error — stream stopped.\n"
                        "Unplug and replug the device if this recurs."));
            break;
        }
        lastGoodRead = samplesRead;

        // ── Raw file ──────────────────────────────────────────────────────────
        if (fileOutputEnabled_ && outFile.is_open()) {
            outFile.write(reinterpret_cast<const char*>(buffer_.data()),
                          static_cast<std::streamsize>(samplesRead) * 2 * sizeof(int16_t));
        }

        // ── Bandpass WAV ──────────────────────────────────────────────────────
        if (bpExp) {
            QVector<int16_t> block(buffer_.data(), buffer_.data() + samplesRead * 2);
            bpExp->pushBlock(block);
        }

        // ── FM demodulator ────────────────────────────────────────────────────
        if (fmDem) {
            // Apply bandwidth change requested by UI (non-blocking, lock-free)
            const double pendingBw = fmBandwidthPending_.exchange(0.0);
            if (pendingBw > 0.0)
                fmDem->setBandwidth(pendingBw);

            QVector<int16_t> block(buffer_.data(), buffer_.data() + samplesRead * 2);
            const QVector<float> audio = fmDem->pushBlock(block);
            if (!audio.isEmpty())
                emit audioReady(audio, fmDem->audioSampleRate());
        }

        // ── FFT throttle ──────────────────────────────────────────────────────
        const auto now = Clock::now();
        if (now - lastPlotTime >= plotInterval_) {
            lastPlotTime = now;
            QVector<int16_t> block(buffer_.data(), buffer_.data() + samplesRead * 2);
            emit samplesReady(std::move(block));
        }
    }

    // ── Завершение ────────────────────────────────────────────────────────────
    device_->teardown_stream();

    if (outFile.is_open()) {
        outFile.flush();
        outFile.close();
    }
    if (bpExp) {
        bpExp->close();
        bpExp.reset();
    }
    fmDem.reset();

    LOG_INFO("StreamWorker finished for: " + serial);
    emit statusMessage(QString("Stream stopped: %1").arg(QString::fromStdString(serial)));
    emit finished();
}
