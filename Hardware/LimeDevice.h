#pragma once

#include "../Core/IDevice.h"
#include "lime/LimeSuite.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// LimeDevice — реализация IDevice для LimeSDR (LimeSuite API).
//
// Поддерживает несколько потоков (RX0, RX1, TX0 ...) на одном устройстве.
// Каждый поток идентифицируется ChannelDescriptor и хранится в streams_.
//
// Поток вызовов:
//   UI thread:     init(), calibrate(), setSampleRate(), setFrequency(), setGain()
//   Worker thread: startStream(ch), readBlock(ch), stopStream(ch)
// ---------------------------------------------------------------------------
class LimeDevice : public IDevice {
    Q_OBJECT

public:
    explicit LimeDevice(const lms_info_str_t& id, QObject* parent = nullptr);
    ~LimeDevice() override;

    LimeDevice(const LimeDevice&)            = delete;
    LimeDevice& operator=(const LimeDevice&) = delete;

    // ── IDevice: идентификация ────────────────────────────────────────────────
    [[nodiscard]] QString id()   const override;
    [[nodiscard]] QString name() const override;

    // ── IDevice: жизненный цикл ───────────────────────────────────────────────
    void init(const QList<ChannelDescriptor>& channels = {}) override;
    void calibrate(const QList<ChannelDescriptor>& channels = {}, double calBwHz = -1.0) override;
    void close()     override;

    // ── IDevice: параметры (глобальные) ──────────────────────────────────────
    void   setSampleRate(double hz)                      override;
    [[nodiscard]] double sampleRate()              const override;
    [[nodiscard]] QList<double> supportedSampleRates()   const override;

    // ── IDevice: параметры (channel 0, backward compat) ──────────────────────
    void   setFrequency(double hz)                       override;
    [[nodiscard]] double frequency()               const override { return currentFrequency_[0]; }

    void   setGain(double dB)                            override;
    [[nodiscard]] double gain()                    const override { return currentGainDb_[0]; }
    [[nodiscard]] double maxGain()                 const override { return kMaxGainDb; }

    // ── IDevice: однокальные стрим (channel 0, backward compat) ──────────────
    void startStream() override;
    void stopStream()  override;
    int  readBlock(int16_t* buffer, int count, int timeoutMs) override;

    // ── IDevice: channel-aware стрим ─────────────────────────────────────────
    void prepareStream(ChannelDescriptor ch) override;
    void startStream(ChannelDescriptor ch)   override;
    void stopStream(ChannelDescriptor ch)    override;
    int  readBlock(ChannelDescriptor ch, int16_t* buffer, int count, int timeoutMs) override;
    void checkPauseForRetune(ChannelDescriptor ch) override;

    // ── IDevice: channel-aware параметры ─────────────────────────────────────
    void setFrequency(ChannelDescriptor ch, double hz) override;
    void setGain(ChannelDescriptor ch, double dB)      override;

    // ── IDevice: hardware timestamps ────────────────────────────────────────
    [[nodiscard]] uint64_t lastReadTimestamp(ChannelDescriptor ch) const override;

    // ── IDevice: TX write ─────────────────────────────────────────────────────
    int writeBlock(ChannelDescriptor ch, const int16_t* buffer,
                   int count, int timeoutMs) override;

    // ── IDevice: состояние ────────────────────────────────────────────────────
    [[nodiscard]] DeviceState state() const override { return state_; }

    // ── IDevice: температура чипа ─────────────────────────────────────────────
    [[nodiscard]] double temperature() const override;

    // ── IDevice: persistence ──────────────────────────────────────────────────
    bool saveConfig(const QString& path) const override;
    bool loadConfig(const QString& path)       override;

    // ── IDevice: возможности устройства ──────────────────────────────────────
    [[nodiscard]] QList<ChannelInfo> availableChannels() const override {
        return {
            {{ChannelDescriptor::RX, 0}, QStringLiteral("RX0")},
            {{ChannelDescriptor::RX, 1}, QStringLiteral("RX1")},
            {{ChannelDescriptor::TX, 0}, QStringLiteral("TX0")},
        };
    }

    // ── IDevice: per-channel getters ─────────────────────────────────────────
    [[nodiscard]] double frequency(ChannelDescriptor ch) const override {
        return ch.direction == ChannelDescriptor::TX
            ? currentTxFrequency_[ch.channelIndex]
            : currentFrequency_[ch.channelIndex];
    }
    [[nodiscard]] double gain(ChannelDescriptor ch) const override {
        return ch.direction == ChannelDescriptor::TX
            ? currentTxGainDb_[ch.channelIndex]
            : currentGainDb_[ch.channelIndex];
    }
    [[nodiscard]] double maxGain(ChannelDescriptor ch) const override {
        return ch.direction == ChannelDescriptor::TX ? kMaxTxGainDb : kMaxGainDb;
    }

    // ── IDevice: аппаратно-специфичный виджет ────────────────────────────────
    QWidget* createAdvancedWidget(QWidget* parent) override;

    static const QList<double> kSupportedRates;

    [[nodiscard]] const lms_info_str_t& limeInfo() const { return deviceId_; }

private:
    void setState(DeviceState s);
    void setupStream(ChannelDescriptor ch);
    void teardownStream(ChannelDescriptor ch);
    void teardownAllStreams();
    void calibrateChannel(int idx, double calBwHz);
    // Retune an actively streaming RX channel. Parks the worker via
    // checkPauseForRetune, stops/reconfigures/restarts the LMS stream
    // (needed to flush the FPGA FIFO), then releases the worker.
    void performStreamingRetune(int rxIdx, double hz);

    lms_device_t*  handle_{nullptr};
    lms_info_str_t deviceId_{};
    std::string    serial_;

    // ── Per-channel state ─────────────────────────────────────────────────────
    // RX index 0/1 = RX channel 0/1.  TX index 0/1 = TX channel 0/1.
    double currentSampleRate_{2'000'000.0};  // global (same for all channels)
    double currentFrequency_[2]    = {102e6, 102e6};  // RX
    double currentGainDb_[2]       = {0.0,   0.0  };  // RX
    double currentTxFrequency_[2]  = {102e6, 102e6};  // TX
    double currentTxGainDb_[2]     = {0.0,   0.0  };  // TX

    // ── Retune handshake (per RX channel) ────────────────────────────────────
    // UI thread setFrequency(ch, hz) on a streaming channel must not race
    // with RxWorker's LMS_RecvStream. Protocol:
    //   1. UI thread: lock mutex, set retuneInProgress_[idx]=true, notify_all.
    //   2. UI thread: wait for workerParked_[idx]==true.
    //   3. Worker (in checkPauseForRetune): sets workerParked_=true, notifies,
    //      waits for retuneInProgress_==false.
    //   4. UI thread: performs LMS_StopStream/SetLO/StartStream, emits retuned()
    //      (DirectConnection → handlers reset DSP state), clears
    //      retuneInProgress_=false, notify_all.
    //   5. Worker: wakes, clears workerParked_, returns from checkPauseForRetune.
    mutable std::mutex              retuneMutex_;
    std::condition_variable         retuneCv_;
    bool                            retuneInProgress_[2] = {false, false};
    bool                            workerParked_[2]     = {false, false};

    // ── Per-channel streams ───────────────────────────────────────────────────
    std::map<ChannelDescriptor, lms_stream_t> streams_;
    std::map<ChannelDescriptor, uint64_t>     lastTimestamp_;  // from lms_stream_meta_t

    DeviceState state_{DeviceState::Connected};

    static constexpr double kMaxGainDb   = 68.5;
    static constexpr double kMaxTxGainDb = 52.0;  // TX PAD + PGA range
    static constexpr int    kDefaultTia  = 3;
};
