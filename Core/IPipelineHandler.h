#pragma once

#include "ChannelDescriptor.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// BlockMeta — metadata attached to every I/Q block dispatched through Pipeline.
//
// channel   — which device channel produced this block
// timestamp — hardware sample counter (from lms_stream_meta_t); 0 if unavailable
// ---------------------------------------------------------------------------
struct BlockMeta {
    ChannelDescriptor channel{};    // default: {RX, 0}
    uint64_t          timestamp{0};
};

// ---------------------------------------------------------------------------
// IPipelineHandler — интерфейс обработчика I/Q блоков в Pipeline.
//
// Правила:
//   • processBlock() вызывается синхронно в потоке StreamWorker.
//     Реализация не должна блокироваться дольше ~1 мс.
//   • Если нужна копия данных (например, для передачи в другой поток),
//     handler делает её сам внутри processBlock().
//   • onStreamStarted() / onStreamStopped() — хуки жизненного цикла,
//     можно не переопределять.
//
// Реализации: FftHandler, FmDemodHandler, RawFileHandler, BandpassHandler,
//             ModClassHandler (будущий AI-классификатор).
// ---------------------------------------------------------------------------
class IPipelineHandler {
public:
    virtual ~IPipelineHandler() = default;

    // Вызывается на каждый I/Q блок.
    // iq    — interleaved int16 [I0, Q0, I1, Q1, ...] (указатель валиден только во время вызова)
    // count — количество I/Q пар (размер буфера = count * 2 * sizeof(int16_t))
    // sampleRateHz — текущая частота дискретизации устройства
    virtual void processBlock(const int16_t* iq, int count, double sampleRateHz) = 0;

    // Extended overload with channel/timestamp metadata.
    // Default implementation calls the basic overload (ignoring meta) —
    // all existing handlers work without modification.
    virtual void processBlock(const int16_t* iq, int count, double sampleRateHz,
                              const BlockMeta& /*meta*/) {
        processBlock(iq, count, sampleRateHz);
    }

    // Вызывается один раз перед первым processBlock() после старта стрима.
    virtual void onStreamStarted(double /*sampleRateHz*/) {}

    // Вызывается один раз после остановки стрима (финализация файлов, flush и т.п.).
    virtual void onStreamStopped() {}
};
