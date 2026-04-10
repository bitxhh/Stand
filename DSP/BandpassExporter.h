#pragma once

#include "DspUtils.h"

#include <QVector>
#include <QString>
#include <cstdint>
#include <complex>
#include <vector>

// ---------------------------------------------------------------------------
// BandpassExporter
//
// DSP chain (all in double, I/Q float32 WAV output):
//
//   int16 I/Q  →  freq-shift to DC  →  FIR lowpass  →  decimate  →  WAV
//
// Designed for capturing a single FM station out of a wideband I/Q stream:
//
//   Input:  raw LimeSDR I/Q blocks (interleaved int16, I first)
//           captured at inputSampleRateHz (e.g. 2 000 000)
//
//   Output: 2-channel float32 WAV (I = left, Q = right)
//           at outputSampleRateHz  (default 250 000 Hz)
//           ready for GNU Radio or MATLAB fm_demod
//
// Usage:
//   BandpassExporter exp(inputSR, stationOffsetHz, bwHz);
//   exp.open("station.wav");
//   // inside RxWorker loop:
//   exp.pushBlock(rawBlock);
//   exp.close();   // flushes WAV header with final sample count
//
// Thread safety: call all methods from the SAME thread (RxWorker thread).
// ---------------------------------------------------------------------------
class BandpassExporter {
public:
    // inputSampleRateHz  — LimeSDR host sample rate (e.g. 2e6)
    // stationOffsetHz    — station frequency RELATIVE to centre freq
    //                      e.g. centre = 102 MHz, station = 104 MHz → offset = +2e6
    //                      Pass 0 if you want the centre frequency itself.
    // bandwidthHz        — one-sided passband; 100 000 is fine for WBFM
    // outputSampleRateHz — decimated output SR; must divide inputSampleRateHz evenly.
    //                      250 000 works for WBFM (≥ 200 kHz required).
    explicit BandpassExporter(double inputSampleRateHz,
                              double stationOffsetHz,
                              double bandwidthHz        = 100'000.0,
                              double outputSampleRateHz = 250'000.0);

    // Open/close the WAV file.  open() may be called only once before close().
    bool open(const QString& path);
    void close();

    // Feed one raw I/Q block.  Does nothing if not open.
    void pushBlock(const QVector<int16_t>& iqBlock);

    // True between open() and close().
    [[nodiscard]] bool isOpen() const { return fileHandle_ != nullptr; }

    // Diagnostics
    [[nodiscard]] int64_t samplesWritten() const { return samplesWritten_; }

private:
    // ── DSP parameters ───────────────────────────────────────────────────────
    double inputSR_;
    double stationOffset_;   // Hz — how far the target station is from LO
    double bandwidth_;       // one-sided lowpass cutoff (Hz)
    double outputSR_;
    int    decimation_;      // inputSR / outputSR, must be integer

    // ── FIR lowpass filter ───────────────────────────────────────────────────
    // Windowed-sinc, designed once in the constructor.
    std::vector<double> firCoeffs_;
    std::vector<std::complex<double>> firDelayLine_;   // circular buffer
    int firHead_{0};

    // ── Frequency-shift NCO ──────────────────────────────────────────────────
    dsp::Nco nco_;

    // ── Decimation state ─────────────────────────────────────────────────────
    int decimationCounter_{0};

    // ── WAV output ───────────────────────────────────────────────────────────
    FILE*   fileHandle_{nullptr};
    int64_t samplesWritten_{0};   // number of (I,Q) pairs written

    // ── Helpers ──────────────────────────────────────────────────────────────
    std::complex<double>       filterSample(std::complex<double> x);

    void writeWavHeader(int64_t numSamples);   // written at open() and patched at close()
    void writeFloat32(float value);
    void patchWavHeader();                     // rewinds and re-writes header with final count
};
