#pragma once

#include "ILogger.h"

#include <QObject>
#include <fstream>
#include <mutex>
#include <string>

// Thread-safe logger singleton.  Implements ILogger so alternative backends
// (e.g. LimeLogger with device-specific context) can replace it later.
//
// Param logging:  LOG_PARAM(LogCat::kPipelineTiming, ms) — filtered by
//                 LoggerConfig; disabled categories are dropped before
//                 any string work.
//
// General logging:  LOG_INFO / LOG_WARN / LOG_ERROR — always recorded.
class Logger : public QObject, public ILogger {
    Q_OBJECT

public:
    static Logger& instance();

    // ILogger — primary entry point (category may be empty)
    void log(LogLevel level, const QString& category, const std::string& msg) override;
    void logParam(const QString& paramKey, double value) override;

    // Backward-compatible overload: empty category
    void log(LogLevel level, const std::string& msg);

    // Convenience wrappers
    void debug(const std::string& msg)   { log(LogLevel::Debug,   msg); }
    void info(const std::string& msg)    { log(LogLevel::Info,    msg); }
    void warning(const std::string& msg) { log(LogLevel::Warning, msg); }
    void error(const std::string& msg)   { log(LogLevel::Error,   msg); }

    void setLogFile(const std::string& path);

signals:
    void logEntryAdded(int level, const QString& category,
                       const QString& timestamp, const QString& message);

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

#define LOG_DEBUG(msg)        Logger::instance().log(LogLevel::Debug,   msg)
#define LOG_INFO(msg)         Logger::instance().log(LogLevel::Info,    msg)
#define LOG_WARN(msg)         Logger::instance().log(LogLevel::Warning, msg)
#define LOG_ERROR(msg)        Logger::instance().log(LogLevel::Error,   msg)
#define LOG_PARAM(key, value) Logger::instance().logParam((key), (value))
