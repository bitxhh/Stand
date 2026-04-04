#pragma once

#include "ITxSource.h"

// ---------------------------------------------------------------------------
// ToneGenerator — ITxSource, генерирующий комплексную CW несущую.
//
// Генерирует: I = A·cos(2π·f·t/SR),  Q = A·sin(2π·f·t/SR)
// где f — toneOffsetHz относительно TX LO, A — amplitude (0..1).
// ---------------------------------------------------------------------------
class ToneGenerator : public ITxSource {
public:
    // toneOffsetHz — сдвиг от TX LO (Гц); 0 = несущая прямо на LO.
    // amplitude    — масштаб int16_t; 1.0 → 32767, рекомендуется ≤ 0.7.
    explicit ToneGenerator(double toneOffsetHz = 0.0, float amplitude = 0.5f);

    void setToneOffset(double hz);
    void setAmplitude(float amp);

    int  generateBlock(int16_t* buffer, int count, double sampleRateHz) override;
    void onTxStarted(double sampleRateHz) override;
    void onTxStopped() override;

private:
    double toneOffsetHz_{0.0};
    float  amplitude_{0.5f};
    double phase_{0.0};   // текущая фаза [0, 2π)
};
