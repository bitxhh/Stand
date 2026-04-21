#pragma once

#include "../Core/ChannelDescriptor.h"
#include "../Core/Pipeline.h"
#include "../DSP/FftHandler.h"
#include "../DSP/BaseDemodHandler.h"
#include "../DSP/RawFileHandler.h"
#include "../DSP/BandpassHandler.h"
#include "../Audio/FmAudioOutput.h"
#include "../Hardware/RxWorker.h"

#include <QObject>
#include <QThread>
#include <QThreadPool>
#include <vector>

class IDevice;

// ---------------------------------------------------------------------------
// RxController — owns Pipeline, RxWorker, handlers, audio output.
//
// No widgets. All cross-thread wiring (QueuedConnections) is set up here.
// DeviceDetailWindow only calls public methods and connects to signals.
// ---------------------------------------------------------------------------
class RxController : public QObject {
    Q_OBJECT

public:
    struct StreamConfig {
        double  loFreqMHz{102.0};
        bool    recordRaw{false};
        QString rawPath;
        bool    exportWav{false};
        QString wavPath;
        double  wavOffset{0.0};
        double  wavBw{100'000.0};
        QString demodMode;        // "" = no demod, "FM", "AM", ...
        double  demodOffsetHz{0.0};
    };

    // channel defaults to {RX, 0} — backward-compatible with existing callers.
    // pool == nullptr → синхронный pipeline (TX, одиночные handlers).
    explicit RxController(IDevice* device,
                           ChannelDescriptor channel = {},
                           QThreadPool* pool = nullptr,
                           QObject* parent = nullptr);
    ~RxController() override;

    // ── Stream lifecycle ─────────────────────────────────────────────────────
    void startStream(const StreamConfig& cfg);
    void stopStream();
    // Synchronous teardown — blocks until worker thread exits (up to 3 s).
    // Call before IDevice::close() to ensure hardware streams are stopped.
    void shutdown() { teardownStream(); }
    [[nodiscard]] bool isStreaming() const { return streamWorker_ != nullptr; }

    // ── Demodulator ──────────────────────────────────────────────────────────
    void setDemodMode(const QString& mode, double offsetHz);
    void teardownDemod();
    void setDemodParam(const QString& name, double value);
    void setDemodOffset(double hz);
    void setVolume(float vol);

    // ── FFT ──────────────────────────────────────────────────────────────────
    void setFftCenterFreq(double mhz);

    // ── Extra handlers (e.g. ClassifierHandler) ──────────────────────────────
    // Safe to call any time; no-op if pipeline is not running.
    void addExtraHandler(IPipelineHandler* h);
    void removeExtraHandler(IPipelineHandler* h);

    // ── Metrics ──────────────────────────────────────────────────────────────
    [[nodiscard]] BaseDemodHandler* demodHandler() const { return demodHandler_; }
    [[nodiscard]] double ifRms() const;

signals:
    void fftReady(FftFrame frame);
    void demodStatus(const QString& msg, bool isError);
    void streamStatus(const QString& msg);
    void streamError(const QString& error);
    void streamFinished();

private:
    void teardownStream();
    void onStreamFinishedInternal();
    void performCleanup();   // shared cleanup body for both paths above
    void onDeviceRetuned(ChannelDescriptor ch, double hz);

    IDevice*          device_;
    ChannelDescriptor channel_{};
    QThreadPool*      pool_{nullptr};
    Pipeline*         pipeline_{nullptr};
    QThread*          streamThread_{nullptr};
    RxWorker*     streamWorker_{nullptr};
    FftHandler*       fftHandler_{nullptr};
    BaseDemodHandler* demodHandler_{nullptr};
    FmAudioOutput*    audioOut_{nullptr};
    float             volume_{0.8f};

    std::vector<RawFileHandler*>   rawHandlers_;
    std::vector<BandpassHandler*>  wavHandlers_;
    std::vector<IPipelineHandler*> extraHandlers_;
};
