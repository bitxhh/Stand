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
//   • processBlock() вызывается из RxWorker thread.
//     При pool != nullptr Pipeline диспатчит каждый handler в отдельную задачу
//     пула и ждёт завершения всех (барьер) — длительная обработка допустима.
//     При pool == nullptr (TX, одиночный handler) вызов синхронный —
//     не блокировать поток дольше необходимого.
//   • Если нужна копия данных (например, для передачи в другой поток),
//     handler делает её сам внутри processBlock().
//   • Handlers не должны разделять изменяемое состояние между собой
//     (нет синхронизации между параллельными вызовами).
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
    // iq    — interleaved float32 [I0, Q0, I1, Q1, ...] нормированный к [-1, 1]
    //         (указатель валиден только во время вызова)
    // count — количество I/Q пар (размер буфера = count * 2 * sizeof(float))
    // sampleRateHz — текущая частота дискретизации устройства
    virtual void processBlock(const float* iq, int count, double sampleRateHz) = 0;

    // Extended overload with channel/timestamp metadata.
    // Default implementation calls the basic overload (ignoring meta) —
    // all existing handlers work without modification.
    virtual void processBlock(const float* iq, int count, double sampleRateHz,
                              const BlockMeta& /*meta*/) {
        processBlock(iq, count, sampleRateHz);
    }

    // Вызывается один раз перед первым processBlock() после старта стрима.
    virtual void onStreamStarted(double /*sampleRateHz*/) {}

    // Вызывается один раз после остановки стрима (финализация файлов, flush и т.п.).
    virtual void onStreamStopped() {}

    // Called when the device LO is retuned while the stream is active.
    // Handlers with accumulated DSP state (NCO phase, FIR delay line,
    // decimation counter) should reset it here — samples after the retune
    // are spectrally discontinuous. Invoked on the UI thread via
    // Pipeline::notifyRetune, synchronously, while the RX worker is parked.
    virtual void onRetune(double /*newFreqHz*/) {}
};
