# LimeSDR Hardware Reference

Configuration mirrors **ExtIO_LimeSDR** (known-good HDSDR plugin reference).

## Init sequence

```
LMS_Open → LMS_Init
→ LMS_GetLOFrequencyRange (log)  → LMS_GetLPFBWRange (log)
→ LMS_SetSampleRate(SR, oversample=2)
→ LMS_EnableChannel(RX+TX) → LMS_SetAntenna(auto by freq)
→ LMS_SetLPFBW(SR) [TIA-protected] → LMS_SetLOFrequency + readback
→ LMS_SetupStream
```

## Key hardware rules

- **Analog LPF BW = sample rate.** Updated on every `setSampleRate()`.
- **TIA protection around LMS_SetLPFBW.** `LMS_SetLPFBW` corrupts G_TIA_RFE internally. Set TIA=3 before, restore after.
- **RCC_CTL_PGA_RBB compensation** after every `setGain()`. Formula (LMS7002M datasheet):
  `rcc = (430 × 0.65^(PGA/10) − 110.35) / 20.4516 + 16`. Without this, PGA filter response degrades.
- **Calibration BW = max(SR, 2.5 MHz).** Matches ExtIO behaviour.
- **Gain to 0 dB before calibration.** Prevents MCU error 5 (LNA loopback at high gain). Restored after.
- **Antenna auto-select:** LNAW < 1.5 GHz, LNAH > 1.5 GHz.
- **Pending frequency via atomic.** During streaming, `setFrequency()` stores value in `pendingFrequency_` (atomic double); `readBlock()` applies on worker thread to avoid USB mutex contention.
- **LO frequency readback.** After `LMS_SetLOFrequency`, read back with `LMS_GetLOFrequency`. Mismatch → warning log (PLL couldn't lock; observed below ~90 MHz on some units).

## Gain structure

Single `setGain(dB)` uses `LMS_SetGaindB` (distributes across LNA + PGA).
TIA managed separately, fixed at `kDefaultTia = 3`.

| Stage | Range | Register |
|-------|-------|----------|
| LNA | 0–30 dB | G_LNA_RFE (1–15) |
| TIA | 0, 3, 12 dB | G_TIA_RFE (1–3), fixed at 3 |
| PGA | 0–31 dB | G_PGA_RBB (0–31) + RCC_CTL_PGA_RBB |

## Logging

Thread-safe singleton `Logger`. Output: `stand.log` next to executable.
Debug builds also mirror to stderr.

```cpp
LOG_INFO("message");  LOG_WARN("message");
LOG_ERROR("message"); LOG_DEBUG("message");
```

Format: `[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message`
