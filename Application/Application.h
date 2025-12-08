#pragma once

#include <QAbstractItemView>
#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>

#include "../LimeCore/LimeManager.h"

class DeviceDetailWindow : public QMainWindow {
    Q_OBJECT
public:
    DeviceDetailWindow(std::shared_ptr<Device> device, LimeManager& manager, QWidget* parent = nullptr);

    signals:
        void deviceDisconnected();

    private slots:
        void checkDeviceConnection();

    private:
        std::shared_ptr<Device> device;
        LimeManager& manager;
        QListWidget* functionList{nullptr};
        QStackedWidget* contentStack{nullptr};
        QWidget* deviceInfoPage{nullptr};
        QTimer* connectionTimer{nullptr};

        QWidget* createDeviceInfoPage();
};

class DeviceSelectionWindow : public QWidget {
    Q_OBJECT
public:
    explicit DeviceSelectionWindow(LimeManager& manager, QWidget* parent = nullptr);

private slots:
    void refreshDevices();
    void openDevice(const std::shared_ptr<Device>& device);

private:
    QLabel* statusLabel{nullptr};
    QListWidget* deviceList{nullptr};
    QTimer* refreshTimer{nullptr};
    LimeManager& manager;
};

class Application {
public:
    Application(int& argc, char** argv, LimeManager& manager);

    int run();

private:
    QApplication qtApp;
    DeviceSelectionWindow selectionWindow;
};