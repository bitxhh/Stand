#include "TxController.h"
#include "../Core/IDevice.h"
#include "../DSP/ToneGenerator.h"
#include "../Hardware/TxWorker.h"
#include "Logger.h"

TxController::TxController(IDevice* device, ChannelDescriptor channel, QObject* parent)
    : QObject(parent)
    , device_(device)
    , channel_(channel)
{}

TxController::~TxController() {
    teardownTx();
}

void TxController::startTx(const TxConfig& cfg) {
    if (txWorker_) return;

    LOG_INFO("TxController::startTx: freq=" + std::to_string(cfg.freqMHz) + " MHz"
             + " gain=" + std::to_string(cfg.gainDb) + " dB"
             + " src=" + cfg.sourceType.toStdString());

    // Set TX LO and gain before starting the stream
    try {
        device_->setFrequency(channel_, cfg.freqMHz * 1e6);
        device_->setGain(channel_, cfg.gainDb);
    } catch (const std::exception& ex) {
        const QString err = QString("TX setup failed: %1").arg(ex.what());
        LOG_ERROR(err.toStdString());
        emit txError(err);
        return;
    }

    // Create source
    auto* tone = new ToneGenerator(cfg.toneOffsetHz, cfg.amplitude);
    txSource_ = tone;

    // Worker thread
    txThread_ = new QThread(this);
    txWorker_ = new TxWorker(device_, txSource_, channel_);
    txWorker_->moveToThread(txThread_);

    connect(txThread_, &QThread::started,  txWorker_, &TxWorker::run);
    connect(txWorker_, &TxWorker::statusMessage,
            this, &TxController::txStatus, Qt::QueuedConnection);
    connect(txWorker_, &TxWorker::errorOccurred,
            this, &TxController::txError, Qt::QueuedConnection);
    connect(txWorker_, &TxWorker::finished,
            this, &TxController::onTxFinishedInternal, Qt::QueuedConnection);
    connect(txWorker_, &TxWorker::finished,
            txThread_, &QThread::quit, Qt::QueuedConnection);
    connect(txThread_, &QThread::finished, txWorker_, &QObject::deleteLater);
    connect(txThread_, &QThread::finished, txThread_, &QObject::deleteLater);

    txThread_->start();
}

void TxController::stopTx() {
    if (txWorker_) txWorker_->stop();
}

void TxController::onTxFinishedInternal() {
    txWorker_ = nullptr;
    txThread_ = nullptr;

    delete txSource_;
    txSource_ = nullptr;

    emit txFinished();
}

void TxController::teardownTx() {
    if (txWorker_) txWorker_->stop();
    if (txThread_) { txThread_->quit(); txThread_->wait(3000); }
    txWorker_ = nullptr;
    txThread_ = nullptr;

    delete txSource_;
    txSource_ = nullptr;
}
