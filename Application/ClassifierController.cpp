#include "ClassifierController.h"
#include "../DSP/ClassifierHandler.h"
#include "RxController.h"
#include "Logger.h"

#include <QJsonDocument>
#include <QJsonObject>

ClassifierController::ClassifierController(RxController* appCtrl, QObject* parent)
    : QObject(parent)
    , appCtrl_(appCtrl)
{}

ClassifierController::~ClassifierController() {
    teardown();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ClassifierController::start(const QString& pythonExe, const QString& scriptPath) {
    if (isRunning()) return;

    LOG_INFO("ClassifierController: starting " + scriptPath.toStdString());

    handler_ = new ClassifierHandler(this);

    socket_ = new QTcpSocket(this);
    connect(socket_, &QTcpSocket::connected,    this, &ClassifierController::onSocketConnected);
    connect(socket_, &QTcpSocket::readyRead,    this, &ClassifierController::onSocketReadyRead);
    connect(socket_, &QTcpSocket::errorOccurred,this, &ClassifierController::onSocketError);

    // Connect handler signal → socket write (cross-thread: worker → main).
    connect(handler_, &ClassifierHandler::frameReady,
            this,     &ClassifierController::sendFrame, Qt::QueuedConnection);

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::started,         this, &ClassifierController::onProcessStarted);
    connect(process_, &QProcess::finished,
            this, &ClassifierController::onProcessFinished);
    connect(process_, &QProcess::readyRead, this, [this]() {
        LOG_INFO("[classifier] " + process_->readAll().trimmed().toStdString());
    });

    process_->start(pythonExe, {scriptPath});
}

void ClassifierController::stop() {
    teardown();
    emit classifierStopped();
}

bool ClassifierController::isRunning() const {
    return process_ && process_->state() != QProcess::NotRunning;
}

// ---------------------------------------------------------------------------
// Process slots
// ---------------------------------------------------------------------------
void ClassifierController::onProcessStarted() {
    LOG_INFO("ClassifierController: process started, connecting to service…");
    connectTimer_ = new QTimer(this);
    connectTimer_->setInterval(500);
    connect(connectTimer_, &QTimer::timeout, this, &ClassifierController::connectToService);
    connectTimer_->start();
    connectToService();  // try immediately
}

void ClassifierController::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    const QString reason = (status == QProcess::CrashExit)
                           ? "crashed"
                           : QString("exited with code %1").arg(exitCode);
    LOG_WARN("ClassifierController: process " + reason.toStdString());

    detachHandler();

    if (connectTimer_) { connectTimer_->stop(); connectTimer_->deleteLater(); connectTimer_ = nullptr; }
    if (socket_)       { socket_->abort(); socket_->deleteLater(); socket_ = nullptr; }
    if (process_)      { process_->deleteLater(); process_ = nullptr; }
    if (handler_)      { handler_->deleteLater(); handler_ = nullptr; }

    emit classifierError("Classifier service " + reason + ".");
    emit classifierStopped();
}

// ---------------------------------------------------------------------------
// Socket connection
// ---------------------------------------------------------------------------
void ClassifierController::connectToService() {
    if (!socket_) return;
    if (socket_->state() != QAbstractSocket::UnconnectedState) return;
    socket_->connectToHost("127.0.0.1", kPort);
}

void ClassifierController::onSocketConnected() {
    LOG_INFO("ClassifierController: connected to classifier service");
    if (connectTimer_) { connectTimer_->stop(); connectTimer_->deleteLater(); connectTimer_ = nullptr; }
    attachHandler();
    emit classifierStarted();
}

void ClassifierController::onSocketError(QAbstractSocket::SocketError /*err*/) {
    // Connection refused = Python not ready yet; retry via connectTimer_.
    // Only log once connected (after classifierStarted was emitted).
    if (!connectTimer_) {
        LOG_WARN("ClassifierController: socket error — "
                 + socket_->errorString().toStdString());
        emit classifierError("Lost connection to classifier service.");
        detachHandler();
    }
    // While connectTimer_ is running: silently retry.
}

// ---------------------------------------------------------------------------
// Data flow: C++ → Python
// ---------------------------------------------------------------------------
void ClassifierController::sendFrame(const QByteArray& data) {
    if (socket_ && socket_->state() == QAbstractSocket::ConnectedState)
        socket_->write(data);
}

// ---------------------------------------------------------------------------
// Data flow: Python → C++
// ---------------------------------------------------------------------------
void ClassifierController::onSocketReadyRead() {
    readBuf_ += socket_->readAll();

    // Responses are newline-terminated JSON objects.
    while (true) {
        const int nl = readBuf_.indexOf('\n');
        if (nl < 0) break;
        const QByteArray line = readBuf_.left(nl).trimmed();
        readBuf_.remove(0, nl + 1);

        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            LOG_WARN("ClassifierController: invalid JSON: " + line.toStdString());
            continue;
        }
        const QJsonObject obj = doc.object();
        const QString type       = obj.value("type").toString("Unknown");
        const double  confidence = obj.value("confidence").toDouble(0.0);
        emit classificationReady(type, confidence);
    }
}

// ---------------------------------------------------------------------------
// Handler pipeline wiring
// ---------------------------------------------------------------------------
void ClassifierController::attachHandler() {
    if (handler_ && appCtrl_)
        appCtrl_->addExtraHandler(handler_);
}

void ClassifierController::detachHandler() {
    if (handler_ && appCtrl_)
        appCtrl_->removeExtraHandler(handler_);
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------
void ClassifierController::teardown() {
    detachHandler();

    if (connectTimer_) { connectTimer_->stop(); delete connectTimer_; connectTimer_ = nullptr; }

    if (socket_) {
        socket_->disconnectFromHost();
        if (socket_->state() != QAbstractSocket::UnconnectedState)
            socket_->waitForDisconnected(500);
        delete socket_;
        socket_ = nullptr;
    }

    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(2000))
            process_->kill();
    }
    delete process_; process_ = nullptr;
    delete handler_; handler_ = nullptr;
}
