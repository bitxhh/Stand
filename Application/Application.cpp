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

    auto* controlItem = new QListWidgetItem("Device control", functionList);
    controlItem->setSizeHint(QSize(0, 48));

    contentStack = new QStackedWidget(central);
    auto* emptyPage = new QWidget(contentStack);
    contentStack->addWidget(emptyPage);

    deviceInfoPage = createDeviceInfoPage();
    deviceControlPage = createDeviceControlPage();
    contentStack->addWidget(deviceInfoPage);
    contentStack->addWidget(deviceControlPage);

    connect(functionList, &QListWidget::itemClicked, this, [this, deviceInfoItem, controlItem](QListWidgetItem* item) {
        if (item == deviceInfoItem) {
            contentStack->setCurrentWidget(deviceInfoPage);
        } else if (item == controlItem) {
            contentStack->setCurrentWidget(deviceControlPage);
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
    connect(&connectionWatcher, &QFutureWatcher<std::vector<std::shared_ptr<Device>>>::finished, this, &DeviceDetailWindow::handleConnectionCheckFinished);
    connectionTimer->start();
    checkDeviceConnection();
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
    if (connectionWatcher.isRunning()) {
        return;
    }

    auto future = QtConcurrent::run([this]() {
        manager.refresh_devices();
        return manager.get_devices();
    });

    connectionWatcher.setFuture(std::move(future));
}

QWidget* DeviceDetailWindow::createDeviceControlPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel("Device control", page);
    title->setStyleSheet("font-weight: 600; font-size: 16px;");

    initStatusLabel = new QLabel("Device not initialized", page);

    sampleRateInput = new QDoubleSpinBox(page);
    sampleRateInput->setDecimals(0);
    sampleRateInput->setRange(1000, 100000000);
    sampleRateInput->setSingleStep(100000);
    sampleRateInput->setValue(5000000);
    sampleRateInput->setSuffix(" Hz");

    auto* initButton = new QPushButton("Initialize device", page);
    auto* calibrateButton = new QPushButton("Calibrate", page);

    connect(initButton, &QPushButton::clicked, this, &DeviceDetailWindow::initializeDevice);
    connect(calibrateButton, &QPushButton::clicked, this, &DeviceDetailWindow::calibrateDevice);

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(initStatusLabel);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Sample rate", page));
    layout->addWidget(sampleRateInput);
    layout->addSpacing(12);
    layout->addWidget(initButton);
    layout->addWidget(calibrateButton);
    layout->addStretch();

    return page;
}

void DeviceDetailWindow::initializeDevice() {
    try {
        device->init_device();
        initStatusLabel->setText("Device initialized");
        QMessageBox::information(this, "Initialization", "Device initialized successfully.");
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, "Initialization error", QString::fromStdString(ex.what()));
    }
}

void DeviceDetailWindow::calibrateDevice() {
    const double sampleRate = sampleRateInput->value();
    try {
        device->calibrate(sampleRate);
        QMessageBox::information(this, "Calibration", "Calibration completed successfully.");
    } catch (const std::exception& ex) {
        QMessageBox::critical(this, "Calibration error", QString::fromStdString(ex.what()));
    }
}

void DeviceDetailWindow::handleConnectionCheckFinished() {
    const auto devices = connectionWatcher.result();
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

    connect(&refreshWatcher, &QFutureWatcher<std::vector<std::shared_ptr<Device>>>::finished, this, [this]() {
        deviceList->clear();

        const auto devices = refreshWatcher.result();
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
    });

    refreshDevices();
    refreshTimer->start();
}

void DeviceSelectionWindow::refreshDevices() {
    if (refreshWatcher.isRunning()) {
        return;
    }

    auto future = QtConcurrent::run([this]() {
        manager.refresh_devices();
        return manager.get_devices();
    });

    refreshWatcher.setFuture(std::move(future));
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