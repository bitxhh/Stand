#pragma once

#include "IPipelineHandler.h"
#include <QObject>
#include <mutex>
#include <shared_mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Pipeline — маршрутизатор I/Q блоков к зарегистрированным обработчикам.
//
// Потокобезопасность:
//   add/remove/clear — можно вызывать из любого потока.
//   dispatch/notify  — вызываются из StreamWorker thread.
//   Список handlers копируется под мьютексом перед вызовом,
//   так что add/remove не блокируют основной цикл.
// ---------------------------------------------------------------------------
class Pipeline : public QObject {
    Q_OBJECT

public:
    explicit Pipeline(QObject* parent = nullptr);

    void addHandler(IPipelineHandler* handler);
    void removeHandler(IPipelineHandler* handler);
    void clearHandlers();

    // Вызывается из StreamWorker thread
    void dispatchBlock(const int16_t* iq, int count, double sampleRateHz);
    void notifyStarted(double sampleRateHz);
    void notifyStopped();

private:
    std::shared_mutex              mutex_;
    std::vector<IPipelineHandler*> handlers_;
};
