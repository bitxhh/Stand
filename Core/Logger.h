#pragma once

#include <QObject>
#include <QString>

#include <fstream>
#include <mutex>
#include <string>

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

// Thread-safe logger: writes to file and emits Qt signal for UI consumption.
// Usage:
//   Logger::instance().log(LogLevel::Info, "Device initialized");
//   Logger::instance().log(LogLevel::Error, "Stream failed: " + detail);
class Logger : public QObject {
    Q_OBJECT

public:
    static Logger& instance();

    // Main logging call — thread-safe, can be called from any thread.
    void log(LogLevel level, const std::string& message);

    // Convenience wrappers
    void debug(const std::string& msg)   { log(LogLevel::Debug,   msg); }
    void info(const std::string& msg)    { log(LogLevel::Info,    msg); }
    void warning(const std::string& msg) { log(LogLevel::Warning, msg); }
    void error(const std::string& msg)   { log(LogLevel::Error,   msg); }

    // Set log file path before first use. Defaults to "stand.log" next to the exe.
    void setLogFile(const std::string& path);

signals:
    // Emitted on every log call — connect to UI widgets from the main thread.
    // level: 0=Debug, 1=Info, 2=Warning, 3=Error
    void logEntryAdded(int level, const QString& timestamp, const QString& message);

private:
    Logger();
    ~Logger() override;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string levelToString(LogLevel level) const;
    std::string currentTimestamp() const;

    std::ofstream logFile_;
    std::mutex    mutex_;
};

// Convenience free-function macros so call sites stay short:
//   LOG_INFO("Device ready");
//   LOG_ERROR("Failed: " + detail);
#define LOG_DEBUG(msg)   Logger::instance().debug(msg)
#define LOG_INFO(msg)    Logger::instance().info(msg)
#define LOG_WARN(msg)    Logger::instance().warning(msg)
#define LOG_ERROR(msg)   Logger::instance().error(msg)
