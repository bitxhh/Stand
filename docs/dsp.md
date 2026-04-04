# DSP Reference

## FM demodulation chain

```
int16 I/Q → DC blocker (IIR HP) → NCO freq-shift
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
| De-emphasis | 50 µs EU default | fc ≈ 3183 Hz |

## AM demodulation chain

```
int16 I/Q → DC blocker (IIR HP) → NCO freq-shift
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

## Supported sample rates

`{2.5, 4, 5, 8, 10, 15, 20}` MS/s — all yield integer D1 for 500 kHz IF.
Debug build max: 10 MS/s. Release uses 255-tap FIR1; Debug uses 31-tap.
