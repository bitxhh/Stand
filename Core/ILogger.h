#pragma once

#include <string>
#include <QString>

enum class LogLevel { Debug, Info, Warning, Error };

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, const QString& category, const std::string& msg) = 0;
    virtual void logParam(const QString& paramKey, double value) = 0;
};
