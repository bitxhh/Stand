#pragma once

#include "../Core/IPipelineHandler.h"
#include "FmDemodulator.h"

#include <QObject>
#include <QVector>
#include <atomic>
#include <memory>

// ---------------------------------------------------------------------------
// FmDemodHandler — WBFM демодуляция, вызывается из StreamWorker thread.
//
// Параметры (stationOffsetHz, deemphTauSec, bandwidthHz) задаются
// в конструкторе и применяются при следующем onStreamStarted().
// setBandwidth() — единственный параметр, меняемый на лету (атомарно).
//
// audioReady() — эмитируется из StreamWorker thread; подключать через
//               Qt::QueuedConnection к FmAudioOutput::push().
// snrDb() / ifRms() — потокобезопасны для чтения из UI thread.
// ---------------------------------------------------------------------------
class FmDemodHandler : public QObject, public IPipelineHandler {
    Q_OBJECT

public:
    explicit FmDemodHandler(double stationOffsetHz = 0.0,
                            double deemphTauSec    = 75e-6,
                            double bandwidthHz     = 100'000.0,
                            QObject* parent        = nullptr);

    // Меняет ширину полосы на лету (применяется в следующем processBlock)
    void setBandwidth(double hz);

    // Метрики качества сигнала — потокобезопасны
    [[nodiscard]] double snrDb() const { return dem_ ? dem_->snrDb() : 0.0; }
    [[nodiscard]] double ifRms() const { return dem_ ? dem_->ifRms() : 0.0; }

    // IPipelineHandler
    void processBlock(const int16_t* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;

signals:
    void audioReady(QVector<float> samples, double sampleRateHz);

private:
    double stationOffsetHz_;
    double deemphTauSec_;
    double bandwidthHz_;

    std::unique_ptr<FmDemodulator> dem_;
    std::atomic<double>            pendingBw_{0.0};   // 0 = нет ожидающего обновления
};
