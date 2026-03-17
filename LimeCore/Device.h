#pragma once
#include "lime/LimeSuite.h"
#include <atomic>
#include <string>

class Device {
public:
    explicit Device(const lms_info_str_t& id);

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    ~Device();

    void init_device();
    void set_sample_rate(double sampleRateHz);
    void calibrate(double sampleRateHz);
    [[nodiscard]] bool is_initialized() const { return is_init; }

    void stream();
    void stopStream() { isRunning = false; }  // можно вызвать из любого потока

    [[nodiscard]] const std::string&    GetSerial() const { return serial; }
    [[nodiscard]] const lms_info_str_t& GetInfo()   const { return device_id; }
    [[nodiscard]] double                get_sample_rate() const;
    static std::string GetDeviceSerial(const lms_info_str_t infoStr);

    bool is_init      = false;
    int  isCalibrated = NotCalibrated;
    std::atomic<bool> isRunning{false};  // atomic — stream() и stopStream() из разных потоков

private:
    friend class StreamWorker;

    void init_stream();

    lms_device_t*  device = nullptr;
    lms_info_str_t device_id{};
    std::string    serial;
    double         currentSampleRate = 0.0;
    lms_stream_t   streamId{};

    static constexpr int sampleCnt = 5000;
    int16_t buffer[sampleCnt * 2]{};

    enum CalibrationStatus {
        Calibrated     =  1,
        NotCalibrated  =  0,
        CalibrationErr = -1
    };
};
