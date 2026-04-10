#pragma once

#include "../Core/ChannelDescriptor.h"
#include <QObject>
#include <QThread>
#include <QString>

class IDevice;
class ITxSource;
class TxWorker;

// ---------------------------------------------------------------------------
// TxController — оркестратор TX: создаёт ITxSource + TxWorker, управляет потоком.
//
// Зеркало RxController для направления TX.
// ---------------------------------------------------------------------------
class TxController : public QObject {
    Q_OBJECT

public:
    struct TxConfig {
        double  freqMHz{102.0};
        double  gainDb{0.0};
        QString sourceType{"Tone"};   // "Tone" — единственный источник в Phase 2
        double  toneOffsetHz{0.0};    // Гц — сдвиг тона от LO
        float   amplitude{0.3f};      // 0.0–1.0; 0.3 — безопасное значение
    };

    explicit TxController(IDevice* device,
                          ChannelDescriptor channel = {ChannelDescriptor::TX, 0},
                          QObject* parent = nullptr);
    ~TxController() override;

    void startTx(const TxConfig& cfg);
    void stopTx();
    [[nodiscard]] bool isTransmitting() const { return txWorker_ != nullptr; }

signals:
    void txStatus(const QString& msg);
    void txError(const QString& error);
    void txFinished();

private:
    void teardownTx();
    void onTxFinishedInternal();

    IDevice*          device_;
    ChannelDescriptor channel_{ChannelDescriptor::TX, 0};
    QThread*          txThread_{nullptr};
    TxWorker*         txWorker_{nullptr};
    ITxSource*        txSource_{nullptr};
};
