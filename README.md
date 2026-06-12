# Spotify Album Art Display (ESP32 + GC9A01)

A round-screen "now playing" display for Spotify. An **ESP32** polls the Spotify
Web API, downloads the current track's album art, decodes the JPEG on-device, and
renders it **full-bleed** on a **240×240 round GC9A01** panel — with a curved
progress ring around the rim and a song/artist label that fades in on track change.

<!-- Add a photo or GIF of the running display here -->
<!-- ![demo](docs/demo.gif) -->

## Features

- 🎨 **Full-bleed album art** — the cover is center-cropped to fill the circle edge to edge.
- ⏱️ **Curved progress ring** — Spotify-green arc hugging the rim, advanced smoothly between polls.
- 🔤 **Song / artist overlay** — appears for a few seconds on a track change, then wipes itself cleanly.
- 🔑 **OAuth2 with auto-refresh** — refreshes the access token ~60 s before it expires.
- 📶 **Resilient networking** — auto-reconnects Wi-Fi; survives `204` (nothing playing), `401` (token expired), and download timeouts without crashing.
- ♻️ **Efficient by design** — keep-alive TLS for polling, no redundant re-downloads, and no full-screen framebuffer (runs comfortably on a **PSRAM-less ESP32**).

## How it works

```
Wi-Fi ─▶ OAuth2 token (refresh grant) ─▶ poll /me/player/currently-playing
      ─▶ extract album-art URL ─▶ stream-download JPEG ─▶ decode ─▶ draw
```

A few deliberate design choices keep it smooth and small:

- **No framebuffer.** `TJpg_Decoder` pushes each decoded block straight to the panel
  via LovyanGFX `pushImage`, so the firmware never holds a 115 KB screen buffer. The
  only large allocation is one reused ~96 KB JPEG download buffer in plain DRAM —
  which is why it works on a bare **ESP32-D0WD with no PSRAM**.
- **Adaptive JPEG scale.** The decoder picks the largest power-of-two scale (1/2/4/8)
  that still fills 240×240, then center-crops — so a 640×640 cover decodes at scale 2
  instead of brute-forcing scale 1.
- **Keep-alive polling.** The currently-playing request reuses a single TLS
  connection, so polls after the first skip the handshake.
- **Incremental progress ring.** Progress is estimated locally between polls and the
  ring only repaints the delta wedge, keeping SPI traffic minimal.
- **Round-safe text.** The overlay fits each line to the circle's chord width at that
  row (`√(r² − dy²)`) and truncates with an ellipsis, so text never clips on the curve.

## Hardware

- **MCU:** ESP32 / ESP32 DevKit (PlatformIO board: `esp32dev`)
- **Display:** GC9A01 240×240 round SPI panel

### Wiring

| GC9A01 pin        | ESP32 GPIO | Notes                              |
|-------------------|------------|------------------------------------|
| `SCL` (clock)     | `18`       | SPI SCLK                           |
| `SDA` (data)      | `23`       | SPI MOSI                           |
| `DC`              | `2`        |                                    |
| `CS`              | `5`        |                                    |
| `RST`             | `4`        |                                    |
| `BLK` (backlight) | 3V3        | or a GPIO — set `PIN_BL` to enable PWM dimming |
| `VCC`             | 3V3 / 5V   | see power note below               |
| `GND`             | GND        | common ground required             |

Pins are defined at the top of [`src/main.cpp`](src/main.cpp).

**Power:** GC9A01 boards are 3.3 V logic. Some modules only light the backlight on
**5 V** due to an onboard regulator — that's fine as long as logic stays 3.3 V and
grounds are common.

## Setup

1. **Create a Spotify app** at the [developer dashboard](https://developer.spotify.com/dashboard),
   note the **Client ID** and **Client Secret**, and add a redirect URI.
2. **Get a refresh token** (one-time) — see below.
3. **Add your secrets:**
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```
   Fill in Wi-Fi + Spotify values. `secrets.h` is git-ignored.
4. **Build & flash** (PlatformIO auto-installs the libraries):
   ```bash
   pio run
   pio run -t upload
   pio device monitor -b 115200
   ```

> **Flashing tip:** if upload fails with *"Wrong boot mode detected"* or *"Failed to
> connect"*, your board's auto-reset isn't triggering. Hold **BOOT**, tap **EN/RST**,
> release **BOOT**, then run the upload — it'll drop into download mode.

### Getting a refresh token

You need a refresh token with the `user-read-currently-playing` scope. The quickest
path is to run the OAuth **Authorization Code** flow once (e.g. with a small local
script or a tool like [Spotify's auth examples](https://github.com/spotify/web-api-examples)),
authorize your account, and exchange the returned `code` for tokens. Copy the
`refresh_token` into `SPOTIFY_REFRESH_TOKEN`. The firmware uses it to mint short-lived
access tokens automatically — you only do this once.

## Configuration

Common knobs at the top of [`src/main.cpp`](src/main.cpp):

| Constant            | Default   | Purpose                                  |
|---------------------|-----------|------------------------------------------|
| `POLL_INTERVAL_MS`  | `8000`    | How often to query Spotify               |
| `OVERLAY_MS`        | `5000`    | How long the song/artist label shows     |
| `SPI_WRITE_FREQ`    | `40 MHz`  | Raise toward 80 MHz on short, clean wiring|
| `MAX_IMAGE_BYTES`   | `96 KB`   | JPEG download buffer cap                  |
| `PIN_*`             | —         | Display pin mapping                       |
| `RING_*`            | —         | Progress-ring radius / colors            |

## Troubleshooting

- **Colors look wrong (red/blue swapped):** set `cfg.rgb_order = true` in the `LGFX` class.
- **Whole image inverted:** toggle `cfg.invert`.
- **Scrambled / noisy pixels:** flip `TJpgDec.setSwapBytes(false)`.
- **`JPEG: decode err N` in serial:** `err 1` means the decode callback aborted — make
  sure the callback returns `true`; `err 2/6+` usually means a truncated/corrupt download.
- **`Image: too big`:** increase `MAX_IMAGE_BYTES` (watch free heap) or prefer a smaller
  image size from the API response.
- **Nothing on screen, serial shows it connects:** check wiring/`PIN_*` and that the
  backlight has power.

## Security

`include/secrets.h` is git-ignored. **If credentials were ever committed or shared,
rotate them immediately:** Wi-Fi password, Spotify client secret, and refresh token.

## License

MIT — see `LICENSE` (add one if you haven't).
