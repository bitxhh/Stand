#pragma once

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "../LimeCore/LimeManager.h"

class DeviceDetailWindow : public QMainWindow {
    Q_OBJECT
public:
    DeviceDetailWindow(std::string serial, std::string info, LimeManager& manager, QWidget* parent = nullptr);

    signals:
        void deviceDisconnected();

private slots:
    void checkDeviceConnection();

private:
    std::string serial;
    LimeManager& manager;
    QTimer* connectionTimer{nullptr};
};

class DeviceSelectionWindow : public QWidget {
    Q_OBJECT
public:
    explicit DeviceSelectionWindow(LimeManager& manager, QWidget* parent = nullptr);

private slots:
    void refreshDevices();
    void openDevice(const std::string& serial, const std::string& info);

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