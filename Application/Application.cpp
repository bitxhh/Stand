#include "Application.h"

#include <algorithm>
#include <QSize>

DeviceDetailWindow::DeviceDetailWindow(std::string serial, std::string info, LimeManager& manager, QWidget* parent)
    : QMainWindow(parent)
    , serial(std::move(serial))
    , manager(manager) {
    setWindowTitle("LimeSDR Device");
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    auto* infoLabel = new QLabel(QString("Serial: %1").arg(QString::fromStdString(this->serial)), central);
    auto* detailLabel = new QLabel(QString("Info: %1").arg(QString::fromStdString(info)), central);

    layout->addWidget(infoLabel);
    layout->addWidget(detailLabel);

    setCentralWidget(central);

    connectionTimer = new QTimer(this);
    connectionTimer->setInterval(1000);
    connect(connectionTimer, &QTimer::timeout, this, &DeviceDetailWindow::checkDeviceConnection);
    connectionTimer->start();
}

void DeviceDetailWindow::checkDeviceConnection() {
    manager.refresh_devices();
    const auto& devices = manager.get_devices();
    const bool connected = std::any_of(devices.begin(), devices.end(), [&](const Device& device) {
        return device.GetSerial() == serial;
    });

    if (!connected) {
        connectionTimer->stop();
        emit deviceDisconnected();
    }
}

DeviceSelectionWindow::DeviceSelectionWindow(LimeManager& manager, QWidget* parent)
    : QWidget(parent)
    , manager(manager) {
    setWindowTitle("Select LimeSDR Device");

    auto* layout = new QVBoxLayout(this);
    statusLabel = new QLabel("Searching for LimeSDR devices...", this);
    deviceList = new QListWidget(this);

    layout->addWidget(statusLabel);
    layout->addWidget(deviceList);

    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(1000);
    connect(refreshTimer, &QTimer::timeout, this, &DeviceSelectionWindow::refreshDevices);

    refreshDevices();
    refreshTimer->start();
}

void DeviceSelectionWindow::refreshDevices() {
    manager.refresh_devices();
    deviceList->clear();

    const auto& devices = manager.get_devices();
    if (devices.empty()) {
        statusLabel->setText("No devices found. Waiting for connection...");
        return;
    }

    statusLabel->setText("Select a device to open its window.");

    for (const auto& device : devices) {
        auto* item = new QListWidgetItem(deviceList);
        item->setSizeHint(QSize(0, 40));

        const std::string serial = device.GetSerial();
        const std::string info = device.GetInfo();

        auto* button = new QPushButton(QString::fromStdString(serial), deviceList);
        connect(button, &QPushButton::clicked, this, [this, serial, info]() {
            openDevice(serial, info);
        });

        deviceList->setItemWidget(item, button);
    }
}

void DeviceSelectionWindow::openDevice(const std::string& serial, const std::string& info) {
    refreshTimer->stop();
    auto* window = new DeviceDetailWindow(serial, info, manager);
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &DeviceDetailWindow::deviceDisconnected, this, [this, window]() {
        QMessageBox::warning(window, "Device disconnected", "Connection to the device was lost. Returning to device search.");
        window->close();
        show();
        refreshTimer->start();
    });

    window->show();
    hide();
}

Application::Application(int& argc, char** argv, LimeManager& manager)
    : qtApp(argc, argv)
    , selectionWindow(manager) {}

int Application::run() {
    selectionWindow.resize(400, 300);
    selectionWindow.show();

    return QApplication::exec();
}