#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>

class RxController;
class ClassifierHandler;

// ---------------------------------------------------------------------------
// ClassifierController — owns the Python subprocess and the TCP connection
// to the classifier service.
//
// Lifecycle:
//   start() → QProcess launches Python script → connectToService() tries to
//   connect (retries until success or process exits) → socket connected →
//   ClassifierHandler added to RxController's pipeline.
//
//   stop() / process crash → ClassifierHandler removed from pipeline →
//   classifierStopped() emitted → UI shows "Unavailable".
//
// All members live on the main thread.
// ClassifierHandler::frameReady is connected via QueuedConnection so the
// worker-thread signal safely reaches this object's sendFrame() slot.
// ---------------------------------------------------------------------------
class ClassifierController : public QObject {
    Q_OBJECT

public:
    explicit ClassifierController(RxController* appCtrl, QObject* parent = nullptr);
    ~ClassifierController() override;

    void start(const QString& pythonExe, const QString& scriptPath);
    void stop();

    [[nodiscard]] bool isRunning() const;

    static constexpr int kPort = 52001;

signals:
    void classificationReady(const QString& type, double confidence);
    void classifierStarted();
    void classifierStopped();
    void classifierError(const QString& msg);

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void connectToService();
    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketError(QAbstractSocket::SocketError err);
    void sendFrame(const QByteArray& data);

private:
    void attachHandler();
    void detachHandler();
    void teardown();

    RxController*     appCtrl_;
    ClassifierHandler* handler_{nullptr};
    QProcess*          process_{nullptr};
    QTcpSocket*        socket_{nullptr};
    QTimer*            connectTimer_{nullptr};   // retry until Python is ready
    QByteArray         readBuf_;
};
