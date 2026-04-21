#pragma once

#include "../Core/IPipelineHandler.h"
#include "BaseDemodulator.h"
#include "DemodTypes.h"

#include <QObject>
#include <QVector>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// BaseDemodHandler — common IPipelineHandler wrapper for all demodulators.
//
// Manages:  generic named parameters (thread-safe), lazy-init,
//           audioReady signal, ifRms proxy.
//
// Subclasses implement:
//   paramDescriptors()  — UI metadata (spin/combo definitions)
//   createDemodulator() — build concrete demodulator from param map
//   applyParam()        — live-update a running demodulator
// ---------------------------------------------------------------------------
class BaseDemodHandler : public QObject, public IPipelineHandler {
    Q_OBJECT

public:
    // Parameter descriptors for UI auto-build.
    virtual std::vector<demod::ParamDesc> paramDescriptors() const { return {}; }

    // Set parameter by name. Thread-safe (UI thread → worker thread).
    void setParam(const QString& name, double value);

    // Read current parameter value. Thread-safe.
    [[nodiscard]] double param(const QString& name) const;

    // Convenience wrappers for common parameters.
    void setBandwidth(double hz) { setParam(QStringLiteral("Bandwidth"), hz); }

    // NCO offset — universal, not a "parameter".
    void setOffset(double hz);

    [[nodiscard]] double ifRms() const { return dem_ ? dem_->ifRms() : 0.0; }

    // IPipelineHandler
    void processBlock(const float* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;

signals:
    void audioReady(QVector<float> samples, double sampleRateHz);

protected:
    explicit BaseDemodHandler(double stationOffsetHz, QObject* parent = nullptr);

    // Subclass creates its concrete demodulator using the param snapshot.
    virtual std::unique_ptr<BaseDemodulator>
    createDemodulator(double sampleRateHz, double offsetHz,
                      const std::map<QString, double>& params) = 0;

    // Apply a single param to a running demodulator.
    // Override to handle live parameter updates.
    virtual void applyParam(BaseDemodulator& dem,
                            const QString& name, double value) {}

    virtual const char* handlerName() const = 0;

    double stationOffsetHz_;
    std::unique_ptr<BaseDemodulator> dem_;

private:
    mutable std::mutex paramMutex_;
    std::map<QString, double> params_;
    std::vector<std::pair<QString, double>> pendingParams_;
    std::atomic<double> pendingOffset_{1e38};  // 1e38 = sentinel "no update"
};
