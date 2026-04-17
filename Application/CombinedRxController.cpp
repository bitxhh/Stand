#include "CombinedRxController.h"
#include "../Core/IDevice.h"
#include "../DSP/DemodRegistry.h"
#include "Logger.h"

CombinedRxController::CombinedRxController(IDevice* device, QThreadPool* pool,
                                             QObject* parent)
    : QObject(parent), device_(device), pool_(pool)
{}

CombinedRxController::~CombinedRxController() {
    teardownStream();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stream lifecycle
// ═══════════════════════════════════════════════════════════════════════════════
void CombinedRxController::startStream(const StreamConfig& cfg) {
    if (!workers_.empty()) return;

    const int nCh = cfg.channels.size();
    if (nCh == 0) return;

    LOG_INFO("CombinedRxController::startStream: " + std::to_string(nCh)
             + " channels, lo=" + std::to_string(cfg.loFreqMHz) + " MHz");

    // ── Combined pipeline (receives merged I/Q) ─────────────────────────────
    combinedPipeline_ = new Pipeline(pool_, this);

    fftHandler_ = new FftHandler(this);
    fftHandler_->setCenterFrequency(cfg.loFreqMHz);
    combinedPipeline_->addHandler(fftHandler_);
    connect(fftHandler_, &FftHandler::fftReady,
            this, &CombinedRxController::fftReady, Qt::QueuedConnection);

    if (cfg.recordRaw) {
        auto* h = new RawFileHandler(cfg.rawPath);
        combinedPipeline_->addHandler(h);
        rawHandlers_.push_back(h);
    }

    if (cfg.exportWav) {
        auto* h = new BandpassHandler(cfg.wavPath, cfg.wavOffset, cfg.wavBw);
        combinedPipeline_->addHandler(h);
        wavHandlers_.push_back(h);
    }

    if (!cfg.demodMode.isEmpty())
        setDemodMode(cfg.demodMode, cfg.demodOffsetHz);

    // ── IqCombiner ──────────────────────────────────────────────────────────
    combiner_ = new IqCombiner(nCh, combinedPipeline_);
    for (int i = 0; i < nCh && i < cfg.gainsDb.size(); ++i)
        combiner_->setChannelGain(i, cfg.gainsDb[i]);

    combinedPipeline_->notifyStarted(device_->sampleRate());

    // ── Per-channel PrePipelines + Workers ──────────────────────────────────
    finishedCount_ = 0;
    workers_.resize(nCh);

    // prepareStream from UI thread before starting any worker (LimeSuite quirk).
    for (int i = 0; i < nCh; ++i)
        device_->prepareStream(cfg.channels[i]);

    for (int i = 0; i < nCh; ++i) {
        auto& w = workers_[i];
        w.channel = cfg.channels[i];

        w.prePipeline = new Pipeline(nullptr, this);
        w.prePipeline->addHandler(combiner_);

        w.thread = new QThread(this);
        w.worker = new RxWorker(device_, w.prePipeline, w.channel);
        w.worker->moveToThread(w.thread);

        connect(w.thread, &QThread::started, w.worker, &RxWorker::run);
        connect(w.worker, &RxWorker::statusMessage,
                this, &CombinedRxController::streamStatus, Qt::QueuedConnection);
        connect(w.worker, &RxWorker::errorOccurred,
                this, &CombinedRxController::streamError, Qt::QueuedConnection);
        connect(w.worker, &RxWorker::finished,
                this, &CombinedRxController::onWorkerFinished, Qt::QueuedConnection);
        connect(w.worker, &RxWorker::finished,
                w.thread, &QThread::quit, Qt::QueuedConnection);
        connect(w.thread, &QThread::finished, w.worker, &QObject::deleteLater);
        connect(w.thread, &QThread::finished, w.thread, &QObject::deleteLater);
    }

    connect(device_, &IDevice::retuned,
            this, &CombinedRxController::onDeviceRetuned, Qt::DirectConnection);

    for (auto& w : workers_)
        w.thread->start();
}

void CombinedRxController::stopStream() {
    for (auto& w : workers_)
        if (w.worker) w.worker->stop();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Demodulator
// ═══════════════════════════════════════════════════════════════════════════════
void CombinedRxController::setDemodMode(const QString& mode, double offsetHz) {
    teardownDemod();
    if (mode.isEmpty() || mode == "Off") return;

    demodHandler_ = DemodRegistry::instance().create(mode, offsetHz, this);
    if (!demodHandler_) return;

    delete audioOut_;
    audioOut_ = new FmAudioOutput(this);
    audioOut_->setVolume(volume_);
    connect(audioOut_, &FmAudioOutput::statusChanged,
            this, &CombinedRxController::demodStatus);
    connect(demodHandler_, &BaseDemodHandler::audioReady,
            audioOut_, &FmAudioOutput::push, Qt::QueuedConnection);

    if (combinedPipeline_)
        combinedPipeline_->addHandler(demodHandler_);
}

void CombinedRxController::teardownDemod() {
    if (combinedPipeline_ && demodHandler_)
        combinedPipeline_->removeHandler(demodHandler_);
    delete demodHandler_;
    demodHandler_ = nullptr;
    if (audioOut_) audioOut_->teardown();
}

void CombinedRxController::setDemodParam(const QString& name, double value) {
    if (demodHandler_) demodHandler_->setParam(name, value);
}

void CombinedRxController::setDemodOffset(double hz) {
    if (demodHandler_) demodHandler_->setOffset(hz);
}

void CombinedRxController::setVolume(float vol) {
    volume_ = vol;
    if (audioOut_) audioOut_->setVolume(vol);
}

// ═══════════════════════════════════════════════════════════════════════════════
// FFT / Metrics / Extra handlers
// ═══════════════════════════════════════════════════════════════════════════════
void CombinedRxController::setFftCenterFreq(double mhz) {
    if (fftHandler_) fftHandler_->setCenterFrequency(mhz);
}

void CombinedRxController::setChannelGain(int channelIndex, double gainDb) {
    if (combiner_) combiner_->setChannelGain(channelIndex, gainDb);
}

void CombinedRxController::addExtraHandler(IPipelineHandler* h) {
    if (!h || !combinedPipeline_) return;
    combinedPipeline_->addHandler(h);
    extraHandlers_.push_back(h);
}

void CombinedRxController::removeExtraHandler(IPipelineHandler* h) {
    if (!h) return;
    if (combinedPipeline_) combinedPipeline_->removeHandler(h);
    extraHandlers_.erase(
        std::remove(extraHandlers_.begin(), extraHandlers_.end(), h),
        extraHandlers_.end());
}

double CombinedRxController::snrDb() const {
    return demodHandler_ ? demodHandler_->snrDb() : 0.0;
}

double CombinedRxController::ifRms() const {
    return demodHandler_ ? demodHandler_->ifRms() : 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Internal cleanup
// ═══════════════════════════════════════════════════════════════════════════════
void CombinedRxController::onWorkerFinished() {
    ++finishedCount_;
    if (finishedCount_ < static_cast<int>(workers_.size())) return;

    // All workers done — reset pointers (deleteLater handles actual deletion).
    for (auto& w : workers_) {
        w.worker = nullptr;
        w.thread = nullptr;
    }
    performCleanup();
    emit streamFinished();
}

void CombinedRxController::teardownStream() {
    // Disconnect finished signals to prevent double-cleanup.
    for (auto& w : workers_) {
        if (w.worker)
            disconnect(w.worker, &RxWorker::finished,
                       this, &CombinedRxController::onWorkerFinished);
    }

    for (auto& w : workers_)
        if (w.worker) w.worker->stop();
    for (auto& w : workers_)
        if (w.thread) { w.thread->quit(); w.thread->wait(3000); }
    for (auto& w : workers_) {
        w.worker = nullptr;
        w.thread = nullptr;
    }

    performCleanup();
}

void CombinedRxController::performCleanup() {
    if (!combinedPipeline_) return;

    for (auto& w : workers_) {
        if (device_) {
            try { device_->stopStream(w.channel); }
            catch (const std::exception& ex) {
                LOG_WARN(std::string("CombinedRxController::performCleanup stopStream: ") + ex.what());
            }
        }
        if (w.prePipeline) {
            w.prePipeline->clearHandlers();
            delete w.prePipeline;
            w.prePipeline = nullptr;
        }
    }
    workers_.clear();

    disconnect(device_, &IDevice::retuned,
               this, &CombinedRxController::onDeviceRetuned);

    combinedPipeline_->notifyStopped();
    combinedPipeline_->clearHandlers();
    delete combinedPipeline_;
    combinedPipeline_ = nullptr;

    delete combiner_;       combiner_      = nullptr;
    delete fftHandler_;     fftHandler_    = nullptr;
    delete demodHandler_;   demodHandler_  = nullptr;

    for (auto* h : rawHandlers_) delete h;
    rawHandlers_.clear();
    for (auto* h : wavHandlers_) delete h;
    wavHandlers_.clear();
    extraHandlers_.clear();

    if (audioOut_) {
        disconnect(audioOut_, nullptr, this, nullptr);
        audioOut_->teardown();
    }

    finishedCount_ = 0;
}

void CombinedRxController::onDeviceRetuned(ChannelDescriptor ch, double hz) {
    // Both RX channels share one RXPLL — react to the first retune only.
    bool ours = false;
    for (const auto& w : workers_) {
        if (w.channel == ch) { ours = true; break; }
    }
    if (!ours) return;

    if (fftHandler_) fftHandler_->setCenterFrequency(hz / 1e6);
    if (combinedPipeline_) combinedPipeline_->notifyRetune(hz);
}
