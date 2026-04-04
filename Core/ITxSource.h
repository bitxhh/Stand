#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// ITxSource — интерфейс источника I/Q данных для TX потока.
//
// Зеркало IPipelineHandler для направления TX:
//   • generateBlock() вызывается синхронно из TxWorker.
//     Должен заполнить buffer и вернуть число I/Q пар (>0), 0 = конец, <0 = ошибка.
//   • onTxStarted() / onTxStopped() — хуки жизненного цикла.
//
// Реализации: ToneGenerator (CW), SilenceSource, FilePlaybackSource (Phase 2+).
// ---------------------------------------------------------------------------
class ITxSource {
public:
    virtual ~ITxSource() = default;

    // Заполняет buffer interleaved int16 I/Q данными.
    // buffer  — размер buffer >= count * 2 * sizeof(int16_t)
    // Возвращает число записанных I/Q пар, 0 = стоп, < 0 = ошибка.
    virtual int generateBlock(int16_t* buffer, int count, double sampleRateHz) = 0;

    virtual void onTxStarted(double /*sampleRateHz*/) {}
    virtual void onTxStopped() {}
};
