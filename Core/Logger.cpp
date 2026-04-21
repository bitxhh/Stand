#include "Logger.h"
#include "LoggerConfig.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::Logger() {
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        std::filesystem::path dir = std::filesystem::path(appdata) / "Stand";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        setLogFile((dir / "stand.log").string());
    } else {
        setLogFile("stand.log");
    }
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.flush();
        logFile_.close();
    }
}

void Logger::setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) logFile_.close();
    logFile_.open(path, std::ios::app);
    if (!logFile_.is_open())
        std::cerr << "[Logger] WARNING: Cannot open log file: " << path << std::endl;
}

void Logger::log(LogLevel level, const std::string& msg) {
    log(level, {}, msg);
}

void Logger::log(LogLevel level, const QString& category, const std::string& msg) {
    if (!category.isEmpty() && !LoggerConfig::instance().isEnabled(category))
        return;

    const std::string ts  = currentTimestamp();
    const std::string lvl = levelToString(level);
    const std::string cat = category.isEmpty()
                            ? ""
                            : "[" + category.toStdString() + "] ";
    const std::string line = "[" + ts + "] [" + lvl + "] " + cat + msg;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (logFile_.is_open()) {
            logFile_ << line << "\n";
            logFile_.flush();
        }
#ifndef NDEBUG
        std::cerr << line << std::endl;
#endif
    }

    emit logEntryAdded(static_cast<int>(level),
                       category,
                       QString::fromStdString(ts),
                       QString::fromStdString("[" + lvl + "] " + cat + msg));
}

void Logger::logParam(const QString& paramKey, double value) {
    if (!LoggerConfig::instance().isEnabled(paramKey))
        return;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << value;
    log(LogLevel::Info, paramKey, paramKey.toStdString() + " = " + oss.str());
}

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO ";
        case LogLevel::Warning: return "WARN ";
        case LogLevel::Error:   return "ERROR";
    }
    return "?????";
}

std::string Logger::currentTimestamp() const {
    using namespace std::chrono;
    const auto now      = system_clock::now();
    const auto ms       = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}
