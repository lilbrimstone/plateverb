# LilBrimstone PlateVerb LV2 🌒

A high-performance Schroeder/Moorer plate reverb capable of massive, lush tails and gritty, lo-fi textures. Designed specifically for the **ISLA Instruments S2400** (but runs on any LV2 host).

**Cpu Efficient:** Mono-In / Stereo-Out architecture optimized for the S2400's ARM processor.

## Features

- **Lush Mod:** Chorus-like modulation on the diffusion stage eliminates metallic ringing and creates wide, expensive-sounding pads.
- **Sync Gate ("Kill The Tank"):** A noise gate that literally kills the internal reverb feedback when closed, preventing "ghost tails" from bleeding into the next hit. Perfect for gated snares.
- **Mud Cut:** High-pass filter (10-1000Hz) applied *before* the reverb tank to keep kicks and basslines clean.
- **Grit:** Soft-clipping saturation stage on the input. Crank it to simulate overdriving vintage hardware inputs.

## Installation (S2400)

1. **Mount S2400:** Connect your S2400 via USB and ensure it is mounted as Drive `D:` in Windows.
2. **Build & Deploy:**
   Run the following from WSL:
   ```bash
   make install_s2400
   ```
   *The Makefile will automatically mount Drive D: to `/mnt/d` if needed.*

3. **Power Cycle:** Reboot your S2400 to clear the generic LV2 cache.

## Controls

| Knob | Parameter | Description |
|------|-----------|-------------|
| 3 | Mix | Dry / Wet balance |
| 4 | PreDelay | 0-200ms delay before reverb starts |
| 5 | Decay | RT60 time (0.1s to 20s) |
| 6 | Damping | High frequency absorption in the tail |
| 7 | Diffusion | Smearing density of the reflections |
| 8 | Size | Room modulation multiplier (0.5x to 1.5x) |
| 9 | Gate | Threshold (0 = Off). Kills feedback when closed. |
| 10 | Mod Depth | LFO excursion (0-5ms) |
| 11 | Mod Rate | LFO speed (0-5Hz) |
| 12 | Low Cut | Pre-reverb HPF (10-1000Hz) |
| 13 | Grit | Input saturation drive |

## License
MIT License