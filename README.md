# Spotify Album Cover (ESP32 + GC9A01)

Round-display Spotify now-playing album art for ESP32 using LovyanGFX + TJpg_Decoder.

## Features
- Auto reconnect Wi-Fi.
- Spotify token refresh with retry-safe handling.
- Handles `204` (nothing playing), `401` (token expired), and download timeouts.
- Only updates the screen when the track or album image changes.
- Smooth vertical reveal transition between album covers.
- GC9A01 round-mask cleanup for clean circular visuals.

## Hardware
- ESP32-C3 SuperMini (PlatformIO board profile: `lolin_c3_mini`)
- GC9A01 240x240 SPI display

Default pin mapping in `src/main.cpp` (ESP32-C3 SuperMini):
- `SCLK=4`
- `MOSI=6`
- `MISO=-1`
- `DC=2`
- `CS=7`
- `RST=3`
- `BL=-1` (set to a pin if your module exposes backlight control)

## Setup
1. Create `include/secrets.h` from `include/secrets.example.h`.
2. Fill Wi-Fi + Spotify values.
3. Build and flash:
   - `pio run`
   - `pio run -t upload`
   - `pio device monitor -b 115200`

## Spotify auth notes
Use Spotify OAuth once to obtain a long-lived refresh token, then put it in `SPOTIFY_REFRESH_TOKEN`.

## Security
`include/secrets.h` is excluded in `.gitignore`.

If credentials were ever committed/shared, rotate immediately:
- Wi-Fi password
- Spotify client secret
- Spotify refresh token
