#pragma once

#include <cstdint>

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

    // Вызывается один раз перед первым processBlock() после старта стрима.
    virtual void onStreamStarted(double /*sampleRateHz*/) {}

    // Вызывается один раз после остановки стрима (финализация файлов, flush и т.п.).
    virtual void onStreamStopped() {}
};
