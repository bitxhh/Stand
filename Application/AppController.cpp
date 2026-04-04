#include "AppController.h"
#include "../DSP/DemodRegistry.h"
#include "Logger.h"

AppController::AppController(IDevice* device, ChannelDescriptor channel, QObject* parent)
    : QObject(parent), device_(device), channel_(channel)
{}

AppController::~AppController() {
    teardownStream();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stream lifecycle
// ═══════════════════════════════════════════════════════════════════════════════
void AppController::startStream(const StreamConfig& cfg) {
    if (streamWorker_) return;

    LOG_INFO("AppController::startStream: lo=" + std::to_string(cfg.loFreqMHz) + " MHz"
             + " record=" + std::to_string(cfg.recordRaw)
             + " wav=" + std::to_string(cfg.exportWav)
             + " mode=" + cfg.demodMode.toStdString());

    pipeline_ = new Pipeline(this);

    // FFT — always active
    fftHandler_ = new FftHandler(this);
    fftHandler_->setCenterFrequency(cfg.loFreqMHz);
    pipeline_->addHandler(fftHandler_);
    connect(fftHandler_, &FftHandler::fftReady,
            this, &AppController::fftReady, Qt::QueuedConnection);

    // Raw recording
    if (cfg.recordRaw) {
        auto* h = new RawFileHandler(cfg.rawPath);
        pipeline_->addHandler(h);
        rawHandlers_.push_back(h);
    }

    // WAV export
    if (cfg.exportWav) {
        auto* h = new BandpassHandler(cfg.wavPath, cfg.wavOffset, cfg.wavBw);
        pipeline_->addHandler(h);
        wavHandlers_.push_back(h);
    }

    // Demodulator
    if (!cfg.demodMode.isEmpty())
        setDemodMode(cfg.demodMode, cfg.demodOffsetHz);

    // Worker thread
    streamThread_ = new QThread(this);
    streamWorker_ = new StreamWorker(device_, pipeline_, channel_);
    streamWorker_->moveToThread(streamThread_);

    connect(streamThread_, &QThread::started,  streamWorker_, &StreamWorker::run);
    connect(streamWorker_, &StreamWorker::statusMessage,
            this, &AppController::streamStatus, Qt::QueuedConnection);
    connect(streamWorker_, &StreamWorker::errorOccurred,
            this, &AppController::streamError, Qt::QueuedConnection);
    connect(streamWorker_, &StreamWorker::finished,
            this, &AppController::onStreamFinishedInternal, Qt::QueuedConnection);
    connect(streamWorker_, &StreamWorker::finished,
            streamThread_, &QThread::quit, Qt::QueuedConnection);
    connect(streamThread_, &QThread::finished, streamWorker_, &QObject::deleteLater);
    connect(streamThread_, &QThread::finished, streamThread_, &QObject::deleteLater);

    streamThread_->start();
}

void AppController::stopStream() {
    if (streamWorker_) streamWorker_->stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Demodulator
// ═══════════════════════════════════════════════════════════════════════════════
void AppController::setDemodMode(const QString& mode, double offsetHz) {
    teardownDemod();
    if (mode.isEmpty() || mode == "Off") return;

    demodHandler_ = DemodRegistry::instance().create(mode, offsetHz, this);
    if (!demodHandler_) return;

    delete audioOut_;
    audioOut_ = new FmAudioOutput(this);
    audioOut_->setVolume(volume_);
    connect(audioOut_, &FmAudioOutput::statusChanged,
            this, &AppController::demodStatus);

    connect(demodHandler_, &BaseDemodHandler::audioReady,
            audioOut_, &FmAudioOutput::push, Qt::QueuedConnection);

    if (pipeline_)
        pipeline_->addHandler(demodHandler_);
}

void AppController::teardownDemod() {
    if (pipeline_ && demodHandler_)
        pipeline_->removeHandler(demodHandler_);
    delete demodHandler_;
    demodHandler_ = nullptr;
    if (audioOut_) audioOut_->teardown();
}

void AppController::setDemodParam(const QString& name, double value) {
    if (demodHandler_) demodHandler_->setParam(name, value);
}

void AppController::setDemodOffset(double hz) {
    if (demodHandler_) demodHandler_->setOffset(hz);
}

void AppController::setVolume(float vol) {
    volume_ = vol;
    if (audioOut_) audioOut_->setVolume(vol);
}

// ═══════════════════════════════════════════════════════════════════════════════
// FFT / Metrics
// ═══════════════════════════════════════════════════════════════════════════════
void AppController::setFftCenterFreq(double mhz) {
    if (fftHandler_) fftHandler_->setCenterFrequency(mhz);
}

void AppController::addExtraHandler(IPipelineHandler* h) {
    if (!h || !pipeline_) return;
    pipeline_->addHandler(h);
    extraHandlers_.push_back(h);
}

void AppController::removeExtraHandler(IPipelineHandler* h) {
    if (!h) return;
    if (pipeline_) pipeline_->removeHandler(h);
    extraHandlers_.erase(
        std::remove(extraHandlers_.begin(), extraHandlers_.end(), h),
        extraHandlers_.end());
}

double AppController::snrDb() const {
    return demodHandler_ ? demodHandler_->snrDb() : 0.0;
}

double AppController::ifRms() const {
    return demodHandler_ ? demodHandler_->ifRms() : 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Internal cleanup
// ═══════════════════════════════════════════════════════════════════════════════
void AppController::onStreamFinishedInternal() {
    // streamWorker/streamThread are deleted via deleteLater
    streamWorker_ = nullptr;
    streamThread_ = nullptr;

    if (pipeline_) {
        pipeline_->clearHandlers();
        delete pipeline_;
        pipeline_ = nullptr;
    }

    delete fftHandler_;      fftHandler_     = nullptr;
    delete demodHandler_;    demodHandler_   = nullptr;

    for (auto* h : rawHandlers_) delete h;
    rawHandlers_.clear();
    for (auto* h : wavHandlers_) delete h;
    wavHandlers_.clear();
    extraHandlers_.clear();   // not owned here — ClassifierController owns them

    if (audioOut_) {
        disconnect(audioOut_, nullptr, this, nullptr);
        audioOut_->teardown();
    }

    emit streamFinished();
}

void AppController::teardownStream() {
    if (streamWorker_) streamWorker_->stop();
    if (streamThread_) { streamThread_->quit(); streamThread_->wait(3000); }
    streamWorker_ = nullptr;
    streamThread_ = nullptr;

    if (pipeline_) {
        pipeline_->clearHandlers();
        delete pipeline_;
        pipeline_ = nullptr;
    }

    delete fftHandler_;      fftHandler_     = nullptr;
    delete demodHandler_;    demodHandler_   = nullptr;

    for (auto* h : rawHandlers_) delete h;
    rawHandlers_.clear();
    for (auto* h : wavHandlers_) delete h;
    wavHandlers_.clear();
    extraHandlers_.clear();   // not owned here

    if (audioOut_) {
        disconnect(audioOut_, nullptr, this, nullptr);
        audioOut_->teardown();
    }
}
