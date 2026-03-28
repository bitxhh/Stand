#include "FmAudioOutput.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
FmAudioOutput::FmAudioOutput(QObject* parent)
    : QObject(parent)
{
    watchdog_ = new QTimer(this);
    watchdog_->setInterval(2000);
    connect(watchdog_, &QTimer::timeout, this, [this]() {
        if (!sink_) return;
        const auto state = sink_->state();
        const auto err   = sink_->error();
        LOG_DEBUG("FmAudioOutput watchdog: state="
                  + std::to_string(static_cast<int>(state))
                  + " error=" + std::to_string(static_cast<int>(err))
                  + " bytesFree=" + std::to_string(sink_->bytesFree())
                  + " outRate=" + std::to_string(outRate_)
                  + " isFloat=" + std::to_string(outIsFloat_));
        // Auto-recover from underrun
        if (state == QAudio::StoppedState && err == QAudio::UnderrunError) {
            LOG_WARN("FmAudioOutput: underrun, restarting");
            device_ = sink_->start();
        }
    });
}

FmAudioOutput::~FmAudioOutput() {
    teardown();
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
void FmAudioOutput::setVolume(float v) {
    volume_ = std::clamp(v, 0.0f, 1.0f);
    if (sink_) sink_->setVolume(volume_);
}

bool FmAudioOutput::isRunning() const {
    return sink_ && (sink_->state() == QAudio::ActiveState
                  || sink_->state() == QAudio::IdleState);
}

void FmAudioOutput::teardown() {
    if (watchdog_) watchdog_->stop();
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_   = nullptr;
        device_ = nullptr;
    }
    openedForRate_ = 0.0;
    outRate_       = 48'000;
    outIsFloat_    = true;
    diagCount_     = 0;
    agcGain_       = 10.0f;
    resampler_.reset();
    statusText_.clear();
    emit statusChanged(QString(), false);
    LOG_DEBUG("FmAudioOutput: torn down");
}

// ---------------------------------------------------------------------------
// push() — called from main thread via QueuedConnection
// ---------------------------------------------------------------------------
void FmAudioOutput::push(QVector<float> samples, double sampleRateHz) {
    if (samples.isEmpty()) return;

    if (!sink_ || openedForRate_ != sampleRateHz) {
        teardown();
        if (!openSink(sampleRateHz)) return;
        openedForRate_ = sampleRateHz;
        watchdog_->start();
    }

    if (!device_) return;

    // Resample demodulator SR → device output rate
    const QVector<float> rs = resampler_.process(
        samples, sampleRateHz, static_cast<double>(outRate_));
    if (rs.isEmpty()) return;

    // ── AGC: compute block RMS, update gain ───────────────────────────────────
    // RMS of current block
    float rms = 0.0f;
    for (float s : rs) rms += s * s;
    rms = std::sqrt(rms / static_cast<float>(rs.size()));

    // Adjust gain toward target; use fast attack / slow release
    if (rms > 0.0001f) {
        const float desiredGain = kAgcTarget / rms;
        const float alpha = (desiredGain < agcGain_) ? kAgcAttack : kAgcRelease;
        agcGain_ = agcGain_ + alpha * (desiredGain - agcGain_);
        agcGain_ = std::clamp(agcGain_, kAgcMin, kAgcMax);
    }

    // Diagnostic every 100 blocks
    if (++diagCount_ % 100 == 0) {
        float peak = 0.0f;
        for (float s : rs) peak = std::max(peak, std::abs(s));
        LOG_DEBUG("FmAudioOutput: rms=" + std::to_string(rms)
                  + " agcGain=" + std::to_string(agcGain_)
                  + " peak_in=" + std::to_string(peak)
                  + " peak_out=" + std::to_string(std::min(peak * agcGain_, 1.0f))
                  + " blocks=" + std::to_string(diagCount_));
    }

    if (outIsFloat_) {
        QVector<float> pcm(rs.size() * 2);
        for (int i = 0; i < rs.size(); ++i) {
            const float s = std::clamp(rs[i] * agcGain_, -1.0f, 1.0f);
            pcm[2 * i]     = s;
            pcm[2 * i + 1] = s;
        }
        const char*  data  = reinterpret_cast<const char*>(pcm.constData());
        const qint64 bytes = static_cast<qint64>(pcm.size()) * sizeof(float);
        const qint64 written = device_->write(data, bytes);
        if (written < bytes)
            LOG_DEBUG("FmAudioOutput: partial write Float32 "
                      + std::to_string(written) + "/" + std::to_string(bytes));
    } else {
        QVector<int16_t> pcm(rs.size() * 2);
        for (int i = 0; i < rs.size(); ++i) {
            const float s = std::clamp(rs[i] * agcGain_, -1.0f, 1.0f);
            const auto  v = static_cast<int16_t>(s * 32767.0f);
            pcm[2 * i]     = v;
            pcm[2 * i + 1] = v;
        }
        const char*  data  = reinterpret_cast<const char*>(pcm.constData());
        const qint64 bytes = static_cast<qint64>(pcm.size()) * sizeof(int16_t);
        const qint64 written = device_->write(data, bytes);
        if (written < bytes)
            LOG_DEBUG("FmAudioOutput: partial write Int16 "
                      + std::to_string(written) + "/" + std::to_string(bytes));
    }
}

// ---------------------------------------------------------------------------
// openSink — always forces Float32 stereo (matches Win10/11 WASAPI default)
// ---------------------------------------------------------------------------
bool FmAudioOutput::openSink(double sampleRateHz) {
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) {
        const QString msg = "FmAudioOutput: no audio output device found";
        LOG_ERROR(msg.toStdString());
        emit statusChanged(msg, true);
        return false;
    }

    // Log preferred format for diagnostics
    const QAudioFormat pref = dev.preferredFormat();
    LOG_INFO("FmAudioOutput: device preferred format: "
             + std::to_string(pref.sampleRate()) + " Hz "
             + std::to_string(pref.channelCount()) + "ch fmt="
             + std::to_string(static_cast<int>(pref.sampleFormat())));

    // Try Float32 stereo at preferred rate first — this matches what WASAPI
    // actually wants on Win10/11 and avoids any silent format conversion.
    QAudioFormat fmt;
    fmt.setSampleRate(pref.sampleRate());
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);

    if (!dev.isFormatSupported(fmt)) {
        // Try Int16 at preferred rate
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(fmt)) {
            // Last resort: 48 kHz Int16 stereo
            fmt.setSampleRate(48'000);
            fmt.setSampleFormat(QAudioFormat::Int16);
        }
    }

    outRate_    = fmt.sampleRate();
    outIsFloat_ = (fmt.sampleFormat() == QAudioFormat::Float);

    LOG_INFO("FmAudioOutput: opening sink — "
             + std::to_string(outRate_) + " Hz stereo "
             + (outIsFloat_ ? "Float32" : "Int16"));

    sink_ = new QAudioSink(dev, fmt, this);

    // Buffer = 300 ms
    const int bytesPerStereoSample = 2 * (outIsFloat_ ? 4 : 2);
    sink_->setBufferSize(
        static_cast<qsizetype>(outRate_ * bytesPerStereoSample * 0.3));
    sink_->setVolume(volume_);

    device_ = sink_->start();

    if (!device_ || sink_->state() == QAudio::StoppedState) {
        const QString errStr = [this]() -> QString {
            switch (sink_->error()) {
                case QAudio::OpenError:     return "OpenError";
                case QAudio::IOError:       return "IOError";
                case QAudio::UnderrunError: return "UnderrunError";
                case QAudio::FatalError:    return "FatalError";
                default:                    return "NoError/Unknown";
            }
        }();
        const QString msg = QString("FmAudioOutput: QAudioSink failed — %1").arg(errStr);
        LOG_ERROR(msg.toStdString());
        emit statusChanged(msg, true);
        delete sink_;
        sink_   = nullptr;
        device_ = nullptr;
        return false;
    }

    statusText_ = QString("FM ♪  |  demod %1 Hz → %2 Hz stereo %3")
        .arg(static_cast<int>(sampleRateHz))
        .arg(outRate_)
        .arg(outIsFloat_ ? "Float32" : "Int16");
    LOG_INFO("FmAudioOutput: sink OK — " + statusText_.toStdString());
    emit statusChanged(statusText_, false);
    return true;
}

// ---------------------------------------------------------------------------
// LinearResampler::process
// ---------------------------------------------------------------------------
QVector<float> FmAudioOutput::LinearResampler::process(
    const QVector<float>& in, double inRate, double outRate)
{
    if (in.isEmpty() || inRate <= 0 || outRate <= 0) return {};

    const double step = inRate / outRate;
    QVector<float> out;
    out.reserve(static_cast<int>(in.size() * outRate / inRate) + 2);

    while (true) {
        const int i = static_cast<int>(phase);
        if (i >= in.size()) {
            phase -= in.size();
            prev = in.back();
            break;
        }
        const float s0   = (i == 0) ? prev : in[i - 1];
        const float s1   = in[i];
        const float frac = static_cast<float>(phase - static_cast<double>(i));
        out.push_back(s0 + frac * (s1 - s0));
        phase += step;
    }
    return out;
}
