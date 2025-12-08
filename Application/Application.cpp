#include "Application.h"

#include <algorithm>
#include <QSize>

DeviceDetailWindow::DeviceDetailWindow(std::shared_ptr<Device> device, LimeManager& manager, QWidget* parent)
    : QMainWindow(parent)
    , device(std::move(device))
    , manager(manager) {
    setWindowTitle(QString::fromStdString(this->device->GetSerial()));

    auto* central = new QWidget(this);
    auto* mainLayout = new QHBoxLayout(central);

    functionList = new QListWidget(central);
    functionList->setFixedWidth(200);
    functionList->setSelectionMode(QAbstractItemView::SingleSelection);
    functionList->setSpacing(4);

    auto* deviceInfoItem = new QListWidgetItem("Device info", functionList);
    deviceInfoItem->setSizeHint(QSize(0, 48));

    contentStack = new QStackedWidget(central);
    auto* emptyPage = new QWidget(contentStack);
    contentStack->addWidget(emptyPage);

    deviceInfoPage = createDeviceInfoPage();
    contentStack->addWidget(deviceInfoPage);

    connect(functionList, &QListWidget::itemClicked, this, [this, deviceInfoItem](QListWidgetItem* item) {
        if (item == deviceInfoItem) {
            contentStack->setCurrentWidget(deviceInfoPage);
        }
    });

    functionList->clearSelection();
    contentStack->setCurrentIndex(0);

    mainLayout->addWidget(functionList);
    mainLayout->addWidget(contentStack, 1);

    setCentralWidget(central);

    connectionTimer = new QTimer(this);
    connectionTimer->setInterval(1000);
    connect(connectionTimer, &QTimer::timeout, this, &DeviceDetailWindow::checkDeviceConnection);
    connectionTimer->start();
}

QWidget* DeviceDetailWindow::createDeviceInfoPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel("Device info", page);
    title->setStyleSheet("font-weight: 600; font-size: 16px;");

    auto* serialLabel = new QLabel(QString("Serial: %1").arg(QString::fromStdString(this->device->GetSerial())), page);
    auto* detailLabel = new QLabel(QString("Info: %1").arg(QString::fromStdString(std::string(this->device->GetInfo()))), page);
    detailLabel->setWordWrap(true);

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(serialLabel);
    layout->addWidget(detailLabel);
    layout->addStretch();

    return page;
}

void DeviceDetailWindow::checkDeviceConnection() {
    manager.refresh_devices();
    const auto& devices = manager.get_devices();
    const bool connected = std::any_of(devices.begin(), devices.end(), [&](const std::shared_ptr<Device>& managedDevice) {
        return managedDevice->GetSerial() == device->GetSerial();
    });

    if (!connected) {
        connectionTimer->stop();
        emit deviceDisconnected();
    }
}

DeviceSelectionWindow::DeviceSelectionWindow(LimeManager& manager, QWidget* parent)
    : QWidget(parent)
    , manager(manager) {
    setWindowTitle("LimeManager VSU");

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

        const std::string serial = device->GetSerial();

        auto* button = new QPushButton(QString::fromStdString(serial), deviceList);
        connect(button, &QPushButton::clicked, this, [this, device]() {
            openDevice(device);
        });

        deviceList->setItemWidget(item, button);
    }
}

void DeviceSelectionWindow::openDevice(const std::shared_ptr<Device>& device) {
    refreshTimer->stop();
    auto* window = new DeviceDetailWindow(device, manager);
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