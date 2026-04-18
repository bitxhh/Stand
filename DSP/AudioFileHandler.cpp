#include "AudioFileHandler.h"
#include "Logger.h"

#include <limits>

namespace {

void writeU32(FILE* f, uint32_t v) {
    uint8_t b[4] = {
        static_cast<uint8_t>( v        & 0xFF),
        static_cast<uint8_t>((v >>  8) & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >> 24) & 0xFF),
    };
    std::fwrite(b, 1, 4, f);
}

void writeU16(FILE* f, uint16_t v) {
    uint8_t b[2] = {
        static_cast<uint8_t>( v       & 0xFF),
        static_cast<uint8_t>((v >> 8) & 0xFF),
    };
    std::fwrite(b, 1, 2, f);
}

}  // namespace

AudioFileHandler::AudioFileHandler(PathBuilder builder, QObject* parent)
    : QObject(parent)
    , builder_(std::move(builder))
{}

AudioFileHandler::~AudioFileHandler() {
    close();
}

void AudioFileHandler::close() {
    if (!file_) return;
    patchWavHeader();
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
    LOG_INFO("AudioFileHandler: closed " + path_.toStdString()
             + " (" + std::to_string(samplesWritten_) + " samples)");
}

bool AudioFileHandler::openFile(double sampleRateHz) {
    path_ = builder_ ? builder_(sampleRateHz) : QString{};
    if (path_.isEmpty()) {
        LOG_ERROR("AudioFileHandler: empty path from builder");
        return false;
    }

    file_ = std::fopen(path_.toStdString().c_str(), "wb");
    if (!file_) {
        LOG_ERROR("AudioFileHandler: cannot open " + path_.toStdString());
        return false;
    }

    openedSampleRate_ = sampleRateHz;
    samplesWritten_   = 0;
    overflowLogged_   = false;

    writeWavHeader(static_cast<uint32_t>(sampleRateHz), 0);
    LOG_INFO("AudioFileHandler: writing mono float32 WAV to " + path_.toStdString()
             + " @ " + std::to_string(static_cast<int>(sampleRateHz)) + " Hz");
    return true;
}

void AudioFileHandler::push(QVector<float> samples, double sampleRateHz) {
    if (samples.isEmpty() || sampleRateHz <= 0.0) return;

    if (!file_) {
        if (!openFile(sampleRateHz)) return;
    } else if (sampleRateHz != openedSampleRate_) {
        LOG_WARN("AudioFileHandler: sample rate changed from "
                 + std::to_string(openedSampleRate_) + " to "
                 + std::to_string(sampleRateHz) + ", dropping block");
        return;
    }

    const std::size_t n = static_cast<std::size_t>(samples.size());
    const std::size_t written =
        std::fwrite(samples.constData(), sizeof(float), n, file_);

    samplesWritten_ += written;

    // WAV header's data-chunk size is uint32 — warn once when we exceed 4 GB
    // of audio payload. Data keeps streaming; the header simply gets clamped.
    constexpr uint64_t kMaxSamples =
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) / sizeof(float);
    if (!overflowLogged_ && samplesWritten_ > kMaxSamples) {
        LOG_WARN("AudioFileHandler: WAV data size exceeds 4 GB, header will be clamped");
        overflowLogged_ = true;
    }
}

void AudioFileHandler::writeWavHeader(uint32_t sampleRate, uint32_t dataBytes) {
    // RIFF/WAVE, IEEE float32, mono.
    const uint16_t numChannels = 1;
    const uint16_t bitsPerSmp  = 32;
    const uint32_t byteRate    = sampleRate * numChannels * (bitsPerSmp / 8);
    const uint16_t blockAlign  = numChannels * (bitsPerSmp / 8);
    const uint32_t chunkSize   = 36 + dataBytes;

    std::fwrite("RIFF", 1, 4, file_);
    writeU32(file_, chunkSize);
    std::fwrite("WAVE", 1, 4, file_);

    std::fwrite("fmt ", 1, 4, file_);
    writeU32(file_, 18);                 // sub-chunk size
    writeU16(file_, 3);                  // audio format: 3 = IEEE float
    writeU16(file_, numChannels);
    writeU32(file_, sampleRate);
    writeU32(file_, byteRate);
    writeU16(file_, blockAlign);
    writeU16(file_, bitsPerSmp);
    writeU16(file_, 0);                  // extra params = 0

    std::fwrite("data", 1, 4, file_);
    writeU32(file_, dataBytes);
}

void AudioFileHandler::patchWavHeader() {
    if (!file_) return;
    const uint64_t total = samplesWritten_ * sizeof(float);
    const uint32_t clamped = total > std::numeric_limits<uint32_t>::max()
                                 ? std::numeric_limits<uint32_t>::max()
                                 : static_cast<uint32_t>(total);
    std::rewind(file_);
    writeWavHeader(static_cast<uint32_t>(openedSampleRate_), clamped);
}
