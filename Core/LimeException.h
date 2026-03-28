#pragma once

#include <stdexcept>
#include <string>

// Base for all LimeSuite-related errors
class LimeException : public std::runtime_error {
public:
    explicit LimeException(const std::string& message)
        : std::runtime_error("[LimeSDR] " + message) {}
};

// Device could not be opened / found
class LimeDeviceOpenException : public LimeException {
public:
    explicit LimeDeviceOpenException(const std::string& serial)
        : LimeException("Failed to open device: " + serial) {}
};

// Init / EnableChannel / SetLOFrequency / SetSampleRate failed
class LimeInitException : public LimeException {
public:
    explicit LimeInitException(const std::string& detail)
        : LimeException("Initialization failed: " + detail) {}
};

// Stream setup / start / receive failed
class LimeStreamException : public LimeException {
public:
    explicit LimeStreamException(const std::string& detail)
        : LimeException("Stream error: " + detail) {}
};

// Calibration failed
class LimeCalibrationException : public LimeException {
public:
    explicit LimeCalibrationException(const std::string& detail)
        : LimeException("Calibration failed: " + detail) {}
};

// Generic parameter validation
class LimeParameterException : public LimeException {
public:
    explicit LimeParameterException(const std::string& detail)
        : LimeException("Invalid parameter: " + detail) {}
};
