# PlateVerb (LilBrimstone)

A lightweight algorithmic reverb LV2 plugin for S2400. Mono-in, stereo-out.
- Algorithm: Schroeder/Moorer (Freeverb-inspired) with HF damping + diffusion
- Controls: Mix, PreDelay (ms), Decay (RT60 s), Damping, Diffusion, Size
- Designed for S2400 LV2 host (strict TTL, safe properties, correct URI)

## Build (native)
make clean && make

## Cross-build for aarch64 (WSL)
# Ensure lv2 headers for aarch64 and pkg-config-aarch64-linux-gnu
# export PKG_CONFIG=pkg-config-aarch64-linux-gnu
make clean && make ARCH=aarch64

## Bundle
make bundle

The built bundle is: plateverb.lv2/
Copy to your S2400 LV2 directory as you normally do.

