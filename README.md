# Spotify Album Cover (ESP32‑D + GC9A01)

Round‑display Spotify now‑playing album art for **ESP32‑D (classic ESP32)** using LovyanGFX + TJpg_Decoder.

## Features
- Auto reconnect Wi‑Fi.
- Spotify token refresh with retry‑safe handling.
- Handles `204` (nothing playing), `401` (token expired), and download timeouts.
- Only updates the screen when the track or album image changes.
- On‑device JPEG decode + draw with centered placement.

## Hardware
- ESP32‑D / ESP32 DevKit (PlatformIO board profile: `esp32dev`)
- GC9A01 240x240 SPI display

Default pin mapping in `src/main.cpp` (ESP32‑D / ESP32 DevKit):
- `SCLK=18`
- `MOSI=23` (often labeled `SDA` on the display)
- `MISO=-1`
- `DC=2`
- `CS=5`
- `RST=4`

Display label mapping:
- `SCL` on the display = `SCLK` (clock)
- `SDA` on the display = `MOSI` (data)

Power notes:
- Most GC9A01 boards are **3.3V logic**.  
- Some modules only show backlight on **5V** because of an onboard regulator. If you use 5V, ensure the board still accepts 3.3V logic (ESP32 GPIO) and that GND is common.

## Setup
1. Create `include/secrets.h` from `include/secrets.example.h`.
2. Fill Wi-Fi + Spotify values.
3. Build and flash:
   - `pio run`
   - `pio run -t upload`
   - `pio device monitor -b 115200`

## Troubleshooting
- If nothing draws, enable the built‑in self‑test in `src/main.cpp` (`DISPLAY_SELF_TEST=true`) to confirm SPI/panel init.
- If you see `Task: failed to allocate image buffer`, reduce `MAX_IMAGE_BYTES` to fit RAM (current default is 120 KB for ESP32‑D).

## Spotify auth notes
Use Spotify OAuth once to obtain a long-lived refresh token, then put it in `SPOTIFY_REFRESH_TOKEN`.

## Security
`include/secrets.h` is excluded in `.gitignore`.

If credentials were ever committed/shared, rotate immediately:
- Wi-Fi password
- Spotify client secret
- Spotify refresh token
