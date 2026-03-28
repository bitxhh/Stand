#pragma once

#include <QList>
<parameter name="content">#include <QObject>
#include <memory>

#include "IDevice.h"

// ---------------------------------------------------------------------------
// IDeviceManager — SDR-агностичный интерфейс менеджера устройств.
//
// Отвечает за:
//   • Обнаружение подключённых устройств (refresh).
//   • Хранение списка активных IDevice.
//   • Уведомление подписчиков при изменении списка (devicesChanged).
//
// Реализации: LimeDeviceManager (LimeSuite), RtlDeviceManager (rtl-sdr), ...
// ---------------------------------------------------------------------------
class IDeviceManager : public QObject {
    Q_OBJECT

public:
    ~IDeviceManager() override = default;

    // Обновить список устройств. Добавляет новые, удаляет исчезнувшие.
    // Эмитирует devicesChanged() если список изменился.
    virtual void refresh() = 0;

    // Текущий список обнаруженных устройств.
    [[nodiscard]] virtual QList<std::shared_ptr<IDevice>> devices() const = 0;

signals:
    // Эмитируется после refresh(), если набор устройств изменился.
    void devicesChanged();
};
