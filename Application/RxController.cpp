#include "RxController.h"
#include "../DSP/DemodRegistry.h"
#include "Logger.h"

RxController::RxController(IDevice* device, ChannelDescriptor channel,
                             QThreadPool* pool, QObject* parent)
    : QObject(parent), device_(device), channel_(channel), pool_(pool)
{}

RxController::~RxController() {
    teardownStream();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stream lifecycle
// ═══════════════════════════════════════════════════════════════════════════════
void RxController::startStream(const StreamConfig& cfg) {
    if (streamWorker_) return;

    LOG_INFO("RxController::startStream: lo=" + std::to_string(cfg.loFreqMHz) + " MHz"
             + " record=" + std::to_string(cfg.recordRaw)
             + " wav=" + std::to_string(cfg.exportWav)
             + " mode=" + cfg.demodMode.toStdString());

    pipeline_ = new Pipeline(pool_, this);

    // FFT — always active
    fftHandler_ = new FftHandler(this);
    fftHandler_->setCenterFrequency(cfg.loFreqMHz);
    pipeline_->addHandler(fftHandler_);
    connect(fftHandler_, &FftHandler::fftReady,
            this, &RxController::fftReady, Qt::QueuedConnection);

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
    streamWorker_ = new RxWorker(device_, pipeline_, channel_);
    streamWorker_->moveToThread(streamThread_);

    connect(streamThread_, &QThread::started,  streamWorker_, &RxWorker::run);
    connect(streamWorker_, &RxWorker::statusMessage,
            this, &RxController::streamStatus, Qt::QueuedConnection);
    connect(streamWorker_, &RxWorker::errorOccurred,
            this, &RxController::streamError, Qt::QueuedConnection);
    connect(streamWorker_, &RxWorker::finished,
            this, &RxController::onStreamFinishedInternal, Qt::QueuedConnection);
    connect(streamWorker_, &RxWorker::finished,
            streamThread_, &QThread::quit, Qt::QueuedConnection);
    connect(streamThread_, &QThread::finished, streamWorker_, &QObject::deleteLater);
    connect(streamThread_, &QThread::finished, streamThread_, &QObject::deleteLater);

    streamThread_->start();
}

void RxController::stopStream() {
    if (streamWorker_) streamWorker_->stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Demodulator
// ═══════════════════════════════════════════════════════════════════════════════
void RxController::setDemodMode(const QString& mode, double offsetHz) {
    teardownDemod();
    if (mode.isEmpty() || mode == "Off") return;

    demodHandler_ = DemodRegistry::instance().create(mode, offsetHz, this);
    if (!demodHandler_) return;

    delete audioOut_;
    audioOut_ = new FmAudioOutput(this);
    audioOut_->setVolume(volume_);
    connect(audioOut_, &FmAudioOutput::statusChanged,
            this, &RxController::demodStatus);

    connect(demodHandler_, &BaseDemodHandler::audioReady,
            audioOut_, &FmAudioOutput::push, Qt::QueuedConnection);

    if (pipeline_)
        pipeline_->addHandler(demodHandler_);
}

void RxController::teardownDemod() {
    if (pipeline_ && demodHandler_)
        pipeline_->removeHandler(demodHandler_);
    delete demodHandler_;
    demodHandler_ = nullptr;
    if (audioOut_) audioOut_->teardown();
}

void RxController::setDemodParam(const QString& name, double value) {
    if (demodHandler_) demodHandler_->setParam(name, value);
}

void RxController::setDemodOffset(double hz) {
    if (demodHandler_) demodHandler_->setOffset(hz);
}

void RxController::setVolume(float vol) {
    volume_ = vol;
    if (audioOut_) audioOut_->setVolume(vol);
}

// ═══════════════════════════════════════════════════════════════════════════════
// FFT / Metrics
// ═══════════════════════════════════════════════════════════════════════════════
void RxController::setFftCenterFreq(double mhz) {
    if (fftHandler_) fftHandler_->setCenterFrequency(mhz);
}

void RxController::addExtraHandler(IPipelineHandler* h) {
    if (!h || !pipeline_) return;
    pipeline_->addHandler(h);
    extraHandlers_.push_back(h);
}

void RxController::removeExtraHandler(IPipelineHandler* h) {
    if (!h) return;
    if (pipeline_) pipeline_->removeHandler(h);
    extraHandlers_.erase(
        std::remove(extraHandlers_.begin(), extraHandlers_.end(), h),
        extraHandlers_.end());
}

double RxController::snrDb() const {
    return demodHandler_ ? demodHandler_->snrDb() : 0.0;
}

double RxController::ifRms() const {
    return demodHandler_ ? demodHandler_->ifRms() : 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Internal cleanup
// ═══════════════════════════════════════════════════════════════════════════════
void RxController::onStreamFinishedInternal() {
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

void RxController::teardownStream() {
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
