# ESP32 Spotify Album Art Display

## Goal
Build a round-screen "now playing" display for Spotify. An ESP32 polls the Spotify
Web API, downloads the current track's album art, decodes the JPEG on-device, and
shows it full-bleed on a 240×240 round GC9A01 panel with a progress ring around the
edge and the song/artist name on track changes.

## Language
C++ (Arduino framework)

## Hardware
- ESP32 development board (`esp32dev`, no PSRAM needed)
- GC9A01 240×240 round SPI display
- Jumper wires
- USB cable for power/flashing

## Software
- PlatformIO (VS Code extension)
- Arduino framework for ESP32
- Libraries (auto-installed from `platformio.ini`):
  - LovyanGFX
  - TJpg_Decoder
  - ArduinoJson (v6)

## Features
- Full-bleed album art, center-cropped to fill the circle
- Curved progress ring around the rim, advanced smoothly between polls
- Song/artist name shown for a few seconds when the track changes
- Spotify OAuth2 with automatic token refresh
- Auto-reconnects Wi-Fi; handles nothing-playing, expired tokens, and timeouts
- Only re-downloads art when the track actually changes
- No framebuffer and a single shared TLS connection, so it runs on a no-PSRAM ESP32

## Pin Mapping

### Display (SPI)
- SCL (clock) → GPIO 18
- SDA (data)  → GPIO 23
- DC → GPIO 2
- CS → GPIO 5
- RST → GPIO 4
- BLK (backlight) → 3V3 (or a GPIO, set `PIN_BL` for PWM dimming)
- VCC → 3V3 / 5V
- GND → GND

Most GC9A01 boards are 3.3V logic. Some only light the backlight on 5V because of an
onboard regulator — that's fine as long as logic stays 3.3V and grounds are common.

## Setup

### 1. Spotify app
Create an app at https://developer.spotify.com/dashboard and note the Client ID and
Client Secret. Run the Authorization Code flow once (with the
`user-read-currently-playing` scope) to get a refresh token — you only do this once,
the firmware mints access tokens from it automatically.

### 2. Secrets
Copy the example and fill in your values (this file is git-ignored):
```
cp include/secrets.example.h include/secrets.h
```

### 3. Build and flash
```
pio run
pio run -t upload
pio device monitor -b 115200
```

## Notes
- If the upload fails to connect, hold **BOOT**, tap **EN/RST**, release **BOOT**, then
  upload — some boards don't auto-reset into download mode.
- If colors look wrong: red/blue swapped → set `cfg.rgb_order = true`; whole image
  inverted → toggle `cfg.invert`; scrambled pixels → `TJpgDec.setSwapBytes(false)`.
- Pins, poll interval, and ring colors are configurable at the top of `src/main.cpp`.

## Security
`include/secrets.h` is git-ignored. If credentials were ever committed or shared,
rotate them: Wi-Fi password, Spotify client secret, and refresh token.
