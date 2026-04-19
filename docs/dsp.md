# DSP Reference

## I/Q format (v2.0)

All `IPipelineHandler::processBlock()` calls receive **float32 interleaved I/Q**
normalised to `[-1, 1]`. Conversion `int16 → float` is done once in `RxWorker`
before the first `PrePipeline` dispatch — no handler ever touches raw int16.

## FM demodulation chain

```
float I/Q → DC blocker (IIR HP) → NCO freq-shift
          → FIR1 LPF (complex, 255 taps, Blackman)  ← push O(1) per sample
          → decimate D1 → IF @ 500 kHz               ← compute O(N) only here
          → FM discriminator (atan2 conjugate product)
          → de-emphasis IIR (τ = 50 µs EU / 75 µs US)
          → FIR2 LPF (real, 255 taps, fc ≈ 15 kHz)
          → decimate D2=10 → audio @ 50 kHz
```

### FM parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| IF target | 500 kHz | D1 = round(inputSR / 500000) |
| Audio SR | 50 kHz | IF / D2 |
| FIR1 taps | 255 (Release) / 31 (Debug) | -55 dB at Nyquist |
| FIR1 bandwidth | 150 kHz default | Adjustable 50–225 kHz |
| FIR2 taps | 255 | Rejects FM stereo subcarrier (23–53 kHz) |
| FM max deviation | ±75 kHz | demodGain = ifSR / (2π × 75000) |
| De-emphasis | 75 µs US default | fc ≈ 2122 Hz |

## AM demodulation chain

```
float I/Q → DC blocker (IIR HP) → NCO freq-shift
          → FIR1 LPF (complex, 255 taps)
          → decimate D1 → IF @ 500 kHz
          → envelope: sqrt(I² + Q²)
          → DC removal (IIR HP ~20 Hz)
          → FIR2 LPF (real, 255 taps, fc ≈ 5 kHz)
          → decimate D2=10 → audio @ 50 kHz
```

### AM parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| IF target | 500 kHz | Same IF architecture as FM |
| Audio SR | 50 kHz | Same FmAudioOutput path |
| FIR1 bandwidth | 5 kHz default | Adjustable 1–20 kHz |
| FIR2 cutoff | ~5 kHz | Matches AM bandwidth |
| DC removal | IIR HP ~20 Hz | Removes carrier DC after sqrt() |

## Why 500 kHz IF

FIR1 must anti-alias before D1 decimation. Transition band = Nyquist − passband:
- 250 kHz IF: transition = 125 − 100 = 25 kHz → ~1000 taps (impractical)
- 500 kHz IF: transition = 250 − 150 = 100 kHz → 255 taps give ~-55 dB

## FIR1 push/compute optimization

Dot product computed only at decimation output points (every D1-th sample).
Between points: O(1) push into delay line. Gives D1× speedup (8× at 4 MS/s).

## FFT (FftProcessor)

Single-precision FFTW3, AVX2+FMA path. Thread-local plan cache (one plan per thread
reused across blocks). ~2× throughput improvement vs double-precision (measured on
Ryzen with AVX2).

## IqCombiner — coherent channel combining

Both RX channels on LimeSDR share one RXPLL (same LO) → coherent I/Q.

```
CH0 block (float32) ──┐
CH1 block (float32) ──┴─ gain normalise each channel (÷ linear gain)
                           ─ sample-by-sample average
                           → combined block → Combined Pipeline
```

Gain normalisation: `scale_n = 1 / 10^(gainDb_n / 20)`.  
Averaging N coherent channels: noise averages down by √N in amplitude → +3 dB SNR per doubling.  
2 channels: +3 dB spectrum display, up to +6 dB demodulator SNR (pre-detection combining).

Timestamp matching uses `BlockMeta::timestamp` (hardware sample counter). If timestamps
differ by more than one block, the older slot is dropped and a new one is waited for.

## BandpassHandler — per-demodulator filtered recording

```
Combined I/Q → NCO shift to VFO offset
             → complex FIR LPF (fc = BW/2)
             → decimate → float32 .cf32 file
```

Written via `BandpassExporter`; output sample rate = inputSR / decimation factor.

## AudioFileHandler — WAV recording

Receives `audioReady(QVector<float>, double sampleRateHz)` from `BaseDemodHandler`.
Writes RIFF/WAVE IEEE-float PCM mono. Sample rate is locked at first block; the WAV
header is patched (seek back) on `close()` with the final sample count.

Filename is determined by a `PathBuilder` closure passed at construction — the closure
receives the audio SR (known only at first block) and composes the path using `FileNaming`.

## Supported sample rates

`{2.5, 4, 5, 8, 10, 15, 20}` MS/s — all yield integer D1 for 500 kHz IF.  
Debug build max: 10 MS/s (31-tap FIR1 — insufficient selectivity above that).  
Release build: 255-tap FIR1 — all rates viable.
