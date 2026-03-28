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
    void set_center_frequency(double freqHz);
    void set_normalized_gain(double gain);   // legacy, kept for compatibility
    void set_gain_db(double gainDb);         // preferred: sets hardware dB directly

    // Stream lifecycle — called by StreamWorker, not the UI.
    // setup_stream() must be called after the final set_sample_rate()
    // and before LMS_StartStream so USB transfer sizes match current SR.
    void setup_stream();
    void teardown_stream();

    [[nodiscard]] bool is_initialized() const { return is_init; }

    void stream();
    void stopStream() { isRunning = false; }

    [[nodiscard]] const std::string&    GetSerial() const { return serial; }
    [[nodiscard]] const lms_info_str_t& GetInfo()   const { return device_id; }
    [[nodiscard]] double                get_sample_rate() const;
    [[nodiscard]] bool                  is_stream_ready() const { return streamReady_; }
    static std::string GetDeviceSerial(const lms_info_str_t infoStr);

    bool is_init      = false;
    int  isCalibrated = NotCalibrated;
    std::atomic<bool> isRunning{false};

private:
    friend class StreamWorker;

    lms_device_t*  device = nullptr;
    lms_info_str_t device_id{};
    std::string    serial;
    double         currentSampleRate = 0.0;
    bool           streamReady_      = false;
    lms_stream_t   streamId{};

    static constexpr int sampleCnt = 5000;
    int16_t buffer[sampleCnt * 2]{};

    enum CalibrationStatus {
        Calibrated     =  1,
        NotCalibrated  =  0,
        CalibrationErr = -1
    };
};