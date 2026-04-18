#pragma once

#include "../Core/ChannelDescriptor.h"
#include "../Core/Pipeline.h"
#include "../Core/RecordingSettings.h"
#include "../DSP/FftHandler.h"
#include "../DSP/BaseDemodHandler.h"
#include "../DSP/RawFileHandler.h"
#include "../DSP/BandpassHandler.h"
#include "../DSP/IqCombiner.h"
#include "../Audio/FmAudioOutput.h"
#include "../Hardware/RxWorker.h"

#include <QObject>
#include <QThread>
#include <QThreadPool>
#include <vector>

class IDevice;

// ---------------------------------------------------------------------------
// CombinedRxController — multi-channel coherent RX with I/Q combining.
//
// Architecture:
//   RxWorker[0] → PrePipeline[0] ──┐
//   RxWorker[1] → PrePipeline[1] ──┴→ IqCombiner → combined Pipeline
//                                                     ├── FftHandler
//                                                     ├── DemodHandler
//                                                     ├── RawFileHandler
//                                                     └── BandpassHandler
//
// API mirrors RxController for UI compatibility.
// Both RX channels share one RXPLL (same LO) on LimeSDR.
// ---------------------------------------------------------------------------
class CombinedRxController : public QObject {
    Q_OBJECT

public:
    struct StreamConfig {
        double  loFreqMHz{102.0};
        QList<ChannelDescriptor> channels;   // e.g. [{RX,0}, {RX,1}]
        QList<double>            gainsDb;    // per-channel gain in dB

        // Combined I/Q capture (after IqCombiner).
        bool    recordRaw{false};
        QString rawPath;

        // Per-channel I/Q capture (before IqCombiner). Index matches `channels`.
        // Empty list or empty entries skip the corresponding channel.
        QList<QString> rawPerChannelPaths;

        // Shared sample format for both combined and per-channel raw captures.
        RecordingSettings::RawFormat rawFormat{RecordingSettings::RawFormat::Float32};

        bool    exportWav{false};
        QString wavPath;
        double  wavOffset{0.0};
        double  wavBw{100'000.0};
        QString demodMode;
        double  demodOffsetHz{0.0};
    };

    explicit CombinedRxController(IDevice* device,
                                   QThreadPool* pool = nullptr,
                                   QObject* parent = nullptr);
    ~CombinedRxController() override;

    void startStream(const StreamConfig& cfg);
    void stopStream();
    void shutdown() { teardownStream(); }
    [[nodiscard]] bool isStreaming() const { return !workers_.empty(); }

    void setDemodMode(const QString& mode, double offsetHz);
    void teardownDemod();
    void setDemodParam(const QString& name, double value);
    void setDemodOffset(double hz);
    void setVolume(float vol);

    void setFftCenterFreq(double mhz);
    void setChannelGain(int channelIndex, double gainDb);

    void addExtraHandler(IPipelineHandler* h);
    void removeExtraHandler(IPipelineHandler* h);

    [[nodiscard]] BaseDemodHandler* demodHandler() const { return demodHandler_; }
    [[nodiscard]] double snrDb() const;
    [[nodiscard]] double ifRms() const;

signals:
    void fftReady(FftFrame frame);
    void demodStatus(const QString& msg, bool isError);
    void streamStatus(const QString& msg);
    void streamError(const QString& error);
    void streamFinished();

private:
    struct WorkerEntry {
        ChannelDescriptor channel;
        Pipeline*         prePipeline{nullptr};
        QThread*          thread{nullptr};
        RxWorker*         worker{nullptr};
        RawFileHandler*   perChannelRaw{nullptr};   // owned here, nullptr if disabled
    };

    void teardownStream();
    void onWorkerFinished();
    void performCleanup();
    void onDeviceRetuned(ChannelDescriptor ch, double hz);

    IDevice*      device_;
    QThreadPool*  pool_{nullptr};

    std::vector<WorkerEntry> workers_;
    IqCombiner*   combiner_{nullptr};
    Pipeline*     combinedPipeline_{nullptr};

    FftHandler*       fftHandler_{nullptr};
    BaseDemodHandler* demodHandler_{nullptr};
    FmAudioOutput*    audioOut_{nullptr};
    float             volume_{0.8f};

    std::vector<RawFileHandler*>   rawHandlers_;
    std::vector<BandpassHandler*>  wavHandlers_;
    std::vector<IPipelineHandler*> extraHandlers_;

    int finishedCount_{0};
};
