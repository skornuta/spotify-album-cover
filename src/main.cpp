// Spotify Album Art Display — ESP32-D + round 240x240 GC9A01 (LovyanGFX)
//
// Flow: connect Wi-Fi -> get/refresh OAuth2 token -> poll currently-playing
//       -> pull album-art JPEG URL -> stream-download -> decode -> render.
//
// UI (Route A, single round display):
//   * Full-bleed album art, center-cropped to fill the circle edge to edge.
//   * Curved Spotify-green progress ring hugging the rim (drawn incrementally).
//   * Song / artist overlay that appears for a few seconds on a track change,
//     then wipes itself by re-decoding only the bottom band of the cached JPEG.
//
// Performance notes (why it stays smooth on a no-PSRAM ESP32-D):
//   * No full-screen framebuffer. TJpg_Decoder pushes decoded blocks straight to
//     the panel; the only large allocation is the JPEG download buffer in DRAM.
//   * The currently-playing poll reuses one keep-alive TLS connection, so polls
//     after the first skip the ~1 s handshake.
//   * Progress between polls is estimated locally; the ring is redrawn only as
//     the filled angle actually changes (incremental fillArc, minimal SPI).
//   * Wi-Fi modem sleep is disabled for consistent, low-latency networking.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <TJpg_Decoder.h>
#include "secrets.h"

// ---- Pins (adjust to your wiring) ----
static constexpr int PIN_SCLK = 18;
static constexpr int PIN_MOSI = 23;   // labeled SDA on most GC9A01 boards
static constexpr int PIN_MISO = -1;   // GC9A01 is write-only here
static constexpr int PIN_DC   = 2;
static constexpr int PIN_CS   = 5;
static constexpr int PIN_RST  = 4;
static constexpr int PIN_BL   = -1;   // set to your backlight pin if wired

// ---- Constants ----
static constexpr uint32_t SPI_WRITE_FREQ   = 40000000;   // 40 MHz; raise to 80M on short wiring
static constexpr uint32_t POLL_INTERVAL_MS = 8000;        // normal poll cadence
static constexpr uint32_t POLL_FAST_MS     = 1500;        // poll cadence when waiting / near track end
static constexpr uint32_t NEAR_END_MS      = 12000;       // "near end" window -> poll fast
static constexpr uint32_t RING_INTERVAL_MS = 250;         // progress ring refresh cadence
static constexpr uint32_t OVERLAY_MS       = 5000;        // how long song/artist shows
static constexpr uint32_t SPIN_INTERVAL_MS = 33;          // loading spinner frame time (~30 fps)
static constexpr uint8_t  REVEAL_ROW_MS    = 9;           // per-row delay for the wipe-in reveal
static constexpr uint32_t HTTP_TIMEOUT_MS  = 12000;
static constexpr uint32_t WIFI_RETRY_MS    = 5000;
static constexpr size_t   MAX_IMAGE_BYTES  = 98304;       // 96 KB; covers seen up to ~60 KB
static constexpr int16_t  DISPLAY_W        = 240;
static constexpr int16_t  DISPLAY_H        = 240;
static constexpr int16_t  CX               = DISPLAY_W / 2;
static constexpr int16_t  CY               = DISPLAY_H / 2;

// Progress ring geometry / colors
static constexpr int      RING_R0    = 112;   // inner radius of ring band
static constexpr int      RING_R1    = 119;   // outer radius of ring band
static constexpr float    RING_START = 270.0; // 12 o'clock (LovyanGFX: 0 deg = 3 o'clock, CW)

// Overlay band
static constexpr int16_t  BAND_TOP   = 152;

static const char *TOKEN_URL   = "https://accounts.spotify.com/api/token";
static const char *CURRENT_URL = "https://api.spotify.com/v1/me/player/currently-playing";

// ---- secrets.h sanity ----
#if !defined(WIFI_SSID) || !defined(WIFI_PASS)
#error "WIFI_SSID / WIFI_PASS missing from secrets.h"
#endif
#if !defined(SPOTIFY_CLIENT_ID) || !defined(SPOTIFY_CLIENT_SECRET) || !defined(SPOTIFY_REFRESH_TOKEN)
#error "SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET / SPOTIFY_REFRESH_TOKEN missing from secrets.h"
#endif

// =====================================================================
//  LovyanGFX device definition for the GC9A01 round panel
// =====================================================================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI      _bus;
  lgfx::Panel_GC9A01 _panel;
  lgfx::Light_PWM    _light;

 public:
  LGFX() {
    {  // SPI bus
      auto cfg = _bus.config();
      cfg.spi_host    = VSPI_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = SPI_WRITE_FREQ;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_SCLK;
      cfg.pin_mosi    = PIN_MOSI;
      cfg.pin_miso    = PIN_MISO;
      cfg.pin_dc      = PIN_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {  // panel
      auto cfg = _panel.config();
      cfg.pin_cs          = PIN_CS;
      cfg.pin_rst         = PIN_RST;
      cfg.pin_busy        = -1;
      cfg.memory_width    = DISPLAY_W;
      cfg.memory_height   = DISPLAY_H;
      cfg.panel_width     = DISPLAY_W;
      cfg.panel_height    = DISPLAY_H;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;
      cfg.readable        = false;
      cfg.invert          = true;   // GC9A01 typically needs inversion ON
      cfg.rgb_order       = false;  // flip to true if R/B look swapped
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel.config(cfg);
    }
    if (PIN_BL >= 0) {  // optional PWM backlight
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_BL;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;

// =====================================================================
//  Global state
// =====================================================================
static uint8_t  *g_imageBuf        = nullptr;  // single reused JPEG download buffer
static size_t    g_imageSize       = 0;        // bytes of the currently-held JPEG
static String    g_accessToken;
static uint32_t  g_tokenExpiresAt  = 0;        // millis() deadline
static String    g_lastTrackId;                // to skip redundant re-downloads
static String    g_trackName;
static String    g_artistName;
static uint32_t  g_progressMs      = 0;
static uint32_t  g_durationMs      = 0;
static uint32_t  g_progressStampMs = 0;        // millis() when progress was sampled

// JPEG placement for the active cover (computed once per image, reused for redraws)
static int16_t   g_jpgX = 0, g_jpgY = 0;
// Vertical clip used by the decode callback (lets us redraw only a band)
static int16_t   g_clipTop = 0, g_clipBot = DISPLAY_H;
// Ring incremental-draw state: filled sweep in degrees currently shown
static float     g_ringDeg = 0.0f;
// When true, the decode callback paces itself row-by-row for a wipe-in reveal.
static bool      g_reveal     = false;
static int16_t   g_revealLastY = INT16_MIN;

// =====================================================================
//  Small helpers: base64 + URL encoding
// =====================================================================
static String base64Encode(const String &in) {
  static const char *tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  const int len = in.length();
  out.reserve(((len + 2) / 3) * 4);
  for (int i = 0; i < len; i += 3) {
    const uint8_t b0 = (uint8_t)in[i];
    const bool h1 = (i + 1) < len, h2 = (i + 2) < len;
    const uint8_t b1 = h1 ? (uint8_t)in[i + 1] : 0;
    const uint8_t b2 = h2 ? (uint8_t)in[i + 2] : 0;
    out += tbl[(b0 >> 2) & 0x3F];
    out += tbl[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
    out += h1 ? tbl[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
    out += h2 ? tbl[b2 & 0x3F] : '=';
  }
  return out;
}

static String urlEncode(const String &in) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() * 3);
  for (size_t i = 0; i < in.length(); ++i) {
    const uint8_t c = (uint8_t)in[i];
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) { out += (char)c; }
    else { out += '%'; out += hex[(c >> 4) & 0x0F]; out += hex[c & 0x0F]; }
  }
  return out;
}

static void configureTls(WiFiClientSecure &client) {
#ifdef SPOTIFY_ROOT_CA
  client.setCACert(SPOTIFY_ROOT_CA);  // pin a CA in secrets.h to validate certs
#else
  client.setInsecure();               // simplest reliable path; fine for this use
#endif
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);
}

// One shared TLS client + HTTP client for ALL Spotify requests. Using a single
// connection means only ONE mbedTLS context is ever live (each costs ~40 KB) --
// critical on a no-PSRAM ESP32. begin() reconnects automatically when the host
// changes, and keep-alive is reused for consecutive same-host polls.
static WiFiClientSecure g_tls;
static HTTPClient       g_http;

static HTTPClient &http() {
  static bool init = false;
  if (!init) {
    configureTls(g_tls);
    g_http.setReuse(true);
    g_http.setTimeout(HTTP_TIMEOUT_MS);
    init = true;
  }
  return g_http;
}

// =====================================================================
//  UI: status screen, progress ring, info overlay
// =====================================================================
static void showStatus(const char *line1, const char *line2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(middle_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.drawString(line1, CX, line2 ? CY - 12 : CY);
  if (line2) {
    tft.setTextColor(tft.color565(150, 150, 150), TFT_BLACK);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.drawString(line2, CX, CY + 14);
  }
}

static float currentRatio() {
  if (g_durationMs == 0) return 0.0f;
  const uint32_t est = g_progressMs + (millis() - g_progressStampMs);
  float r = (float)est / (float)g_durationMs;
  return r < 0 ? 0 : (r > 1 ? 1 : r);
}

// Full redraw of the ring (dim track + green sweep). Used after (re)drawing art.
static void ringFull(float ratio) {
  const uint16_t track = tft.color565(45, 45, 45);
  const uint16_t fill  = tft.color565(0x1D, 0xB9, 0x54);
  tft.fillArc(CX, CY, RING_R0, RING_R1, 0, 360, track);
  g_ringDeg = 360.0f * ratio;
  if (g_ringDeg > 0.5f)
    tft.fillArc(CX, CY, RING_R0, RING_R1, RING_START, RING_START + g_ringDeg, fill);
}

// Incremental ring update: only paint the delta wedge since last frame.
static void ringUpdate(float ratio) {
  const float deg = 360.0f * ratio;
  if (deg > g_ringDeg + 1.0f) {                       // advanced
    const uint16_t fill = tft.color565(0x1D, 0xB9, 0x54);
    tft.fillArc(CX, CY, RING_R0, RING_R1,
                RING_START + g_ringDeg, RING_START + deg, fill);
    g_ringDeg = deg;
  } else if (deg < g_ringDeg - 1.0f) {                // seeked backward
    ringFull(ratio);
  }
}

// Largest half-width that fits inside the circle at screen row y.
static int chordHalfWidth(int y, int r = 116) {
  const int dy = y - CY;
  const int v  = r * r - dy * dy;
  return v <= 0 ? 0 : (int)sqrtf((float)v);
}

// Draw one centered line, truncating with an ellipsis to fit maxW.
static void drawFitted(const String &s, int y, int maxW) {
  String t = s;
  if (tft.textWidth(t) > maxW) {
    while (t.length() > 1 && tft.textWidth(t + "...") > maxW) t.remove(t.length() - 1);
    t += "...";
  }
  tft.drawString(t, CX, y);
}

static void drawInfoOverlay() {
  tft.fillRect(0, BAND_TOP, DISPLAY_W, DISPLAY_H - BAND_TOP, TFT_BLACK);  // scrim
  tft.setTextDatum(top_center);
  tft.setTextWrap(false);

  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE);
  drawFitted(g_trackName, BAND_TOP + 6, 2 * chordHalfWidth(BAND_TOP + 14));

  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(tft.color565(170, 170, 170));
  drawFitted(g_artistName, BAND_TOP + 30, 2 * chordHalfWidth(BAND_TOP + 38));
}

// =====================================================================
//  TJpg_Decoder callback — push decoded block straight to the panel
// =====================================================================
static bool jpgToScreen(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  // Always return true: a false return aborts the whole decode (JDR_INTR).
  // Skip blocks outside the active vertical clip; pushImage clips x for us.
  if (y + (int)h <= g_clipTop || y >= g_clipBot) return true;
  tft.pushImage(x, y, w, h, bitmap);
  // Reveal mode: brief pause when we move to a new row of blocks, so the new
  // cover wipes in top-to-bottom instead of popping in all at once.
  if (g_reveal && y != g_revealLastY) {
    g_revealLastY = y;
    tft.endWrite(); delay(REVEAL_ROW_MS); tft.startWrite();  // flush row, then pause
  }
  return true;
}

// =====================================================================
//  Wi-Fi
// =====================================================================
static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  static uint32_t lastAttempt = 0;
  if (lastAttempt != 0 && millis() - lastAttempt < WIFI_RETRY_MS) return false;
  lastAttempt = millis();

  Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);            // no modem sleep -> steady, low-latency polls
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi: connect failed");
  return false;
}

// =====================================================================
//  OAuth2 token (refresh-token grant)
// =====================================================================
static bool refreshToken() {
  Serial.println("Token: refreshing...");
  const String basic =
      base64Encode(String(SPOTIFY_CLIENT_ID) + ":" + String(SPOTIFY_CLIENT_SECRET));
  const String body =
      "grant_type=refresh_token&refresh_token=" + urlEncode(String(SPOTIFY_REFRESH_TOKEN));

  HTTPClient &h = http();
  if (!h.begin(g_tls, TOKEN_URL)) return false;
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  h.addHeader("Authorization", "Basic " + basic);

  const int code = h.POST(body);
  if (code != 200) {
    Serial.printf("Token: HTTP %d -> %s\n", code, h.getString().c_str());
    h.end();
    return false;
  }

  StaticJsonDocument<128> filter;
  filter["access_token"] = true;
  filter["expires_in"]   = true;
  StaticJsonDocument<512> doc;
  const DeserializationError err =
      deserializeJson(doc, h.getStream(), DeserializationOption::Filter(filter));
  h.end();
  if (err) { Serial.printf("Token: JSON err %s\n", err.c_str()); return false; }

  const char *tok = doc["access_token"];
  if (!tok || !tok[0]) return false;
  const uint32_t expiresIn = doc["expires_in"] | 3600;

  g_accessToken    = tok;
  g_tokenExpiresAt = millis() + expiresIn * 1000UL;
  Serial.printf("Token: ok, expires in %lus\n", (unsigned long)expiresIn);
  return true;
}

static bool ensureToken() {
  // Refresh if absent or within 60 s of expiry (signed cast handles millis() wrap).
  if (!g_accessToken.isEmpty() &&
      (int32_t)(g_tokenExpiresAt - millis()) > 60000) {
    return true;
  }
  return refreshToken();
}

// =====================================================================
//  Currently-playing: reuses one keep-alive TLS connection
// =====================================================================
enum class NowPlaying { Error, Nothing, Track };

static NowPlaying fetchNowPlaying(String &trackId, String &albumUrl) {
  trackId = "";
  albumUrl = "";

  HTTPClient &h = http();
  if (!h.begin(g_tls, CURRENT_URL)) return NowPlaying::Error;
  h.addHeader("Authorization", "Bearer " + g_accessToken);

  const int code = h.GET();
  if (code == 204) { h.end(); return NowPlaying::Nothing; }       // nothing playing
  if (code == 401) {                                              // token died early
    h.end();
    g_accessToken.clear();
    g_tokenExpiresAt = 0;
    return NowPlaying::Error;
  }
  if (code != 200) {
    Serial.printf("NowPlaying: HTTP %d\n", code);
    h.end();
    return NowPlaying::Error;
  }

  // Filter keeps only the few fields we need so the doc stays tiny.
  StaticJsonDocument<320> filter;
  filter["progress_ms"]                       = true;
  filter["item"]["id"]                        = true;
  filter["item"]["name"]                      = true;
  filter["item"]["duration_ms"]               = true;
  filter["item"]["artists"][0]["name"]        = true;
  filter["item"]["album"]["images"][0]["url"] = true;
  filter["item"]["album"]["images"][1]["url"] = true;
  filter["item"]["album"]["images"][2]["url"] = true;

  StaticJsonDocument<1024> doc;
  const DeserializationError err =
      deserializeJson(doc, h.getStream(), DeserializationOption::Filter(filter));
  h.end();
  if (err) { Serial.printf("NowPlaying: JSON err %s\n", err.c_str()); return NowPlaying::Error; }

  const char *id = doc["item"]["id"] | "";
  if (!id[0]) return NowPlaying::Nothing;  // e.g. a podcast episode / ad

  JsonArray imgs = doc["item"]["album"]["images"].as<JsonArray>();
  if (imgs.isNull() || imgs.size() == 0) return NowPlaying::Nothing;
  // Prefer the 300x300 (index 1); fall back to whatever exists.
  const char *url = imgs[imgs.size() > 1 ? 1 : 0]["url"] | "";
  if (!url[0]) return NowPlaying::Nothing;

  trackId           = id;
  albumUrl          = url;
  g_trackName       = doc["item"]["name"]               | "";
  g_artistName      = doc["item"]["artists"][0]["name"] | "";
  g_progressMs      = doc["progress_ms"]                | 0;
  g_durationMs      = doc["item"]["duration_ms"]        | 0;
  g_progressStampMs = millis();
  return NowPlaying::Track;
}

// =====================================================================
//  Download JPEG into g_imageBuf
// =====================================================================
static bool downloadImage(const String &url) {
  g_imageSize = 0;
  HTTPClient &h = http();
  if (!h.begin(g_tls, url)) return false;

  const int code = h.GET();
  if (code != 200) { Serial.printf("Image: HTTP %d\n", code); h.end(); return false; }

  const int len = h.getSize();
  if (len > (int)MAX_IMAGE_BYTES) {
    Serial.printf("Image: too big (%d > %u)\n", len, (unsigned)MAX_IMAGE_BYTES);
    h.end();
    return false;
  }

  WiFiClient *stream = h.getStreamPtr();
  size_t      got    = 0;
  uint32_t    lastRx = millis();
  while (h.connected() && (len < 0 || (int)got < len)) {
    const size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail;
      if (got + toRead > MAX_IMAGE_BYTES) toRead = MAX_IMAGE_BYTES - got;
      if (toRead == 0) break;
      const int n = stream->readBytes(g_imageBuf + got, toRead);
      if (n > 0) { got += n; lastRx = millis(); }
    } else {
      if (millis() - lastRx > HTTP_TIMEOUT_MS) break;
      delay(2);
    }
  }
  h.end();

  if (got == 0) { Serial.println("Image: empty"); return false; }
  g_imageSize = got;
  Serial.printf("Image: %u bytes\n", (unsigned)got);
  return true;
}

// =====================================================================
//  Decode + render the cached JPEG (whole screen, or just a vertical band)
// =====================================================================
static bool decodeBand(int16_t top, int16_t bottom, bool reveal = false) {
  if (g_imageSize == 0) return false;

  uint16_t w = 0, h = 0;
  if (TJpgDec.getJpgSize(&w, &h, g_imageBuf, g_imageSize) != JDR_OK || w == 0 || h == 0) {
    Serial.println("JPEG: bad header");
    return false;
  }
  // Largest power-of-two scale (1/2/4/8) that still fills 240x240.
  uint8_t scale = 1;
  while (scale < 8 &&
         (w / (scale * 2)) >= DISPLAY_W &&
         (h / (scale * 2)) >= DISPLAY_H) {
    scale *= 2;
  }
  TJpgDec.setJpgScale(scale);

  const int16_t sw = w / scale, sh = h / scale;
  g_jpgX = (DISPLAY_W - sw) / 2;     // negative -> center-crop
  g_jpgY = (DISPLAY_H - sh) / 2;
  g_clipTop = top;
  g_clipBot = bottom;
  g_reveal      = reveal;
  g_revealLastY = INT16_MIN;

  tft.startWrite();
  const JRESULT r = TJpgDec.drawJpg(g_jpgX, g_jpgY, g_imageBuf, g_imageSize);
  tft.endWrite();

  g_clipTop = 0;
  g_clipBot = DISPLAY_H;
  g_reveal  = false;
  if (r != JDR_OK) { Serial.printf("JPEG: decode err %d\n", (int)r); return false; }
  return true;
}

static bool renderAlbumArt() {
  const bool ok = decodeBand(0, DISPLAY_H, /*reveal=*/true);
  if (ok) Serial.printf("JPEG: rendered, art at %d,%d\n", g_jpgX, g_jpgY);
  return ok;
}

// =====================================================================
//  Waiting state: animated loading spinner instead of a blank screen
// =====================================================================
static const char *g_waitLabel = nullptr;   // current waiting label (nullptr => not waiting)

static void clearWaiting() { g_waitLabel = nullptr; }   // call once art is shown

static void serviceWaiting(const char *label) {
  static float    ang  = 0.0f;
  static uint32_t last = 0;
  if (label != g_waitLabel) {                // new state -> repaint background + label
    g_waitLabel = label;
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(tft.color565(150, 150, 150), TFT_BLACK);
    tft.drawString(label, CX, CY + 60);
  }
  if (millis() - last < SPIN_INTERVAL_MS) return;
  last = millis();
  const int r0 = 26, r1 = 34;                // small centered loader ring
  tft.fillArc(CX, CY, r0, r1, 0, 360, tft.color565(28, 28, 28));
  tft.fillArc(CX, CY, r0, r1, ang, ang + 80, tft.color565(0x1D, 0xB9, 0x54));
  ang += 11;
  if (ang >= 360) ang -= 360;
}

// Poll faster while waiting for music or when the current track is about to end,
// so a new song shows up in ~1.5 s instead of after the full 8 s poll cycle.
static uint32_t pollInterval(bool haveArt) {
  if (!haveArt) return POLL_FAST_MS;
  if (g_durationMs > 0) {
    const uint32_t est = g_progressMs + (millis() - g_progressStampMs);
    if (est + NEAR_END_MS >= g_durationMs) return POLL_FAST_MS;
  }
  return POLL_INTERVAL_MS;
}

// =====================================================================
//  Arduino entry points
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nSpotify Album Art Display");

  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);

  TJpgDec.setJpgScale(1);          // overridden per-image in decodeBand()
  TJpgDec.setSwapBytes(true);      // RGB565 byte order for LovyanGFX pushImage
  TJpgDec.setCallback(jpgToScreen);

  g_imageBuf = (uint8_t *)malloc(MAX_IMAGE_BYTES);
  if (!g_imageBuf) {
    showStatus("Out of memory", "JPEG buffer");
    Serial.println("FATAL: cannot allocate image buffer");
    while (true) delay(1000);
  }

  showStatus("Starting", "Connecting Wi-Fi");
}

void loop() {
  static uint32_t lastPoll     = 0;
  static uint32_t lastRing     = 0;
  static uint32_t overlayUntil = 0;
  static bool     haveArt      = false;
  static const char *waitMsg   = "Waiting for music";

  if (!ensureWifi()) {
    haveArt = false;
    serviceWaiting("Connecting Wi-Fi");
    delay(10);
    return;
  }

  // --- Poll Spotify (cadence speeds up while waiting / near track end) ---
  if (lastPoll == 0 || millis() - lastPoll >= pollInterval(haveArt)) {
    lastPoll = millis();

    if (!ensureToken()) {
      haveArt = false;
      waitMsg = "Connecting Spotify";
    } else {
      String trackId, albumUrl;
      switch (fetchNowPlaying(trackId, albumUrl)) {
        case NowPlaying::Error:
          if (!haveArt) waitMsg = "Reconnecting";   // keep art on screen if we have it
          break;

        case NowPlaying::Nothing:
          haveArt = false;
          g_lastTrackId = "";
          waitMsg = "Waiting for music";
          break;

        case NowPlaying::Track:
          if (trackId != g_lastTrackId) {           // new track -> fetch + wipe in
            if (downloadImage(albumUrl) && renderAlbumArt()) {
              g_lastTrackId = trackId;
              haveArt = true;
              clearWaiting();
              drawInfoOverlay();                     // show song / artist
              overlayUntil = millis() + OVERLAY_MS;
              Serial.printf("Rendered: %s — %s\n", g_trackName.c_str(), g_artistName.c_str());
            } else {
              haveArt = false;
              waitMsg = "Art unavailable";
            }
          }
          break;
      }
    }
  }

  if (haveArt) {
    // Auto-clear the overlay by re-decoding only the bottom band.
    if (overlayUntil && millis() >= overlayUntil) {
      overlayUntil = 0;
      decodeBand(BAND_TOP, DISPLAY_H);
      if (g_durationMs > 0) ringFull(currentRatio());
    }
    // Animate the progress ring (only while art is clean of the overlay).
    if (!overlayUntil && g_durationMs > 0 && millis() - lastRing >= RING_INTERVAL_MS) {
      lastRing = millis();
      ringUpdate(currentRatio());
    }
  } else {
    serviceWaiting(waitMsg);                          // animated spinner, no blank screen
  }

  delay(10);
}
