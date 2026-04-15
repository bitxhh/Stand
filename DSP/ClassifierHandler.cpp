#include "ClassifierHandler.h"

#include <QSysInfo>
#include <cstring>

ClassifierHandler::ClassifierHandler(QObject* parent)
    : QObject(parent)
{}

void ClassifierHandler::setIntervalMs(int ms) {
    intervalMs_.store(ms);
}

// ---------------------------------------------------------------------------
// IPipelineHandler
// ---------------------------------------------------------------------------
void ClassifierHandler::processBlock(const float* iq, int count, double sampleRateHz) {
    processBlock(iq, count, sampleRateHz, BlockMeta{});
}

void ClassifierHandler::processBlock(const float* iq, int count,
                                     double sampleRateHz, const BlockMeta& meta)
{
    const auto now = Clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - lastEmit_).count();
    if (elapsedMs < intervalMs_.load()) return;
    lastEmit_ = now;

    emit frameReady(serialize(iq, count, sampleRateHz, meta.timestamp));
}

// ---------------------------------------------------------------------------
// Frame serialization — little-endian throughout
// ---------------------------------------------------------------------------
QByteArray ClassifierHandler::serialize(const float* iq, int count,
                                        double sampleRateHz, uint64_t timestamp)
{
    const int32_t n          = static_cast<int32_t>(count);
    // NOTE: frame protocol change — I/Q payload is now float32 (4B/sample)
    // Python classifier must be updated to unpack with np.frombuffer(..., dtype=np.float32)
    const uint32_t payloadLen = static_cast<uint32_t>(8 + 4 + 8 + n * 2 * sizeof(float));

    QByteArray buf;
    buf.reserve(static_cast<qsizetype>(4 + payloadLen));

    auto appendU32 = [&](uint32_t v) {
        char b[4];
        b[0] = static_cast<char>(v & 0xFF);
        b[1] = static_cast<char>((v >> 8) & 0xFF);
        b[2] = static_cast<char>((v >> 16) & 0xFF);
        b[3] = static_cast<char>((v >> 24) & 0xFF);
        buf.append(b, 4);
    };
    auto appendU64 = [&](uint64_t v) {
        char b[8];
        for (int i = 0; i < 8; ++i)
            b[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
        buf.append(b, 8);
    };
    auto appendI32 = [&](int32_t v) {
        appendU32(static_cast<uint32_t>(v));
    };
    auto appendF64 = [&](double v) {
        char b[8]; std::memcpy(b, &v, 8);  // native (x86 = LE)
        buf.append(b, 8);
    };

    appendU32(payloadLen);
    appendU64(timestamp);
    appendI32(n);
    appendF64(sampleRateHz);
    buf.append(reinterpret_cast<const char*>(iq),
               static_cast<qsizetype>(n * 2 * sizeof(float)));
    return buf;
}
