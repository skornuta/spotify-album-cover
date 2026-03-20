#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <TJpg_Decoder.h>
#include "secrets.h"
#include "esp_task_wdt.h"

using namespace lgfx;

// ---- Pins / constants ----
static constexpr int PIN_SCLK = 18;
static constexpr int PIN_MOSI = 23;
static constexpr int PIN_MISO = -1;
static constexpr int PIN_DC   = 2;
static constexpr int PIN_CS   = 5;
static constexpr int PIN_RST  = 4;

static constexpr uint32_t SPI_WRITE_FREQ       = 20000000;
static constexpr uint32_t SPI_READ_FREQ        = 8000000;
static constexpr uint32_t POLL_INTERVAL_MS     = 15000;
static constexpr uint32_t WIFI_RETRY_DELAY_MS  = 10000;
static constexpr uint32_t HTTP_TIMEOUT_MS      = 15000;
static constexpr size_t   MAX_IMAGE_BYTES      = 131072;
static constexpr uint8_t  JPG_SCALE            = 2;

static const char *TOKEN_URL   = "https://accounts.spotify.com/api/token";
static const char *CURRENT_URL = "https://api.spotify.com/v1/me/player/currently-playing";
static constexpr bool DISPLAY_SELF_TEST = true;

// ---- Secrets checks ----
#ifndef WIFI_SSID
#error "WIFI_SSID is missing."
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS is missing."
#endif
#ifndef SPOTIFY_CLIENT_ID
#error "SPOTIFY_CLIENT_ID is missing."
#endif
#ifndef SPOTIFY_CLIENT_SECRET
#error "SPOTIFY_CLIENT_SECRET is missing."
#endif
#ifndef SPOTIFY_REFRESH_TOKEN
#error "SPOTIFY_REFRESH_TOKEN is missing."
#endif

// ---- LovyanGFX display ----
class LGFX : public LGFX_Device {
 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = VSPI_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = SPI_WRITE_FREQ;
      cfg.freq_read   = SPI_READ_FREQ;
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
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = PIN_CS;
      cfg.pin_rst          = PIN_RST;
      cfg.pin_busy         = -1;
      cfg.memory_width     = 240;
      cfg.memory_height    = 240;
      cfg.panel_width      = 240;
      cfg.panel_height     = 240;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 0;
      cfg.dummy_read_bits  = 0;
      cfg.readable         = false;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
 private:
  Bus_SPI      _bus;
  Panel_GC9A01 _panel;
};

static LGFX tft;

// ---- Global state ----
static int16_t  g_imageX        = 0;
static int16_t  g_imageY        = 0;
static float    g_spinAngle     = 0.0f;
static uint16_t *g_frameBuf     = nullptr;

static String   g_accessToken;
static uint32_t g_tokenExpiresAtMs = 0;
static String   g_lastAlbumUrl;
static String   g_lastTrackId;

// NEW: track metadata and timing
static String   g_lastTrackName;
static String   g_lastArtistName;
static uint32_t g_progressMs   = 0;
static uint32_t g_durationMs   = 0;
static uint32_t g_lastUpdateMs = 0;

// ---- Helpers: base64 + URL encode ----
static String base64Encode(const String &in) {
  static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  const int len = in.length();
  out.reserve(((len + 2) / 3) * 4);
  for (int i = 0; i < len; i += 3) {
    const uint8_t b0 = static_cast<uint8_t>(in[i]);
    const bool hasB1 = (i + 1) < len;
    const bool hasB2 = (i + 2) < len;
    const uint8_t b1 = hasB1 ? static_cast<uint8_t>(in[i + 1]) : 0;
    const uint8_t b2 = hasB2 ? static_cast<uint8_t>(in[i + 2]) : 0;
    out += tbl[(b0 >> 2) & 0x3F];
    out += tbl[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
    out += hasB1 ? tbl[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
    out += hasB2 ? tbl[b2 & 0x3F] : '=';
  }
  return out;
}

static String urlEncode(const String &in) {
  String out;
  out.reserve(in.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < in.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(in[i]);
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// ---- TJpg_Decoder callback — writes into g_frameBuf ----
static bool tftJpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  for (uint16_t row = 0; row < h; row++) {
    int16_t destY = g_imageY + y + row;
    if (destY < 0 || destY >= 240) continue;
    for (uint16_t col = 0; col < w; col++) {
      int16_t destX = g_imageX + x + col;
      if (destX < 0 || destX >= 240) continue;
      g_frameBuf[destY * 240 + destX] = bitmap[row * w + col];
    }
  }
  return true;
}

// NEW: simple status screen
static void drawStatusScreen(const char *line1, const char *line2) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(middle_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2 - 8);
  if (line2 && line2[0]) {
    tft.setTextColor(tft.color565(160,160,160), TFT_BLACK);
    tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 10);
  }
}

// NEW: track title / artist overlay
static void drawTrackOverlay()
{
  const int16_t w    = tft.width();
  const int16_t h    = tft.height();
  const int16_t barH = 40;

  uint16_t barColor = tft.color565(10, 10, 10);
  tft.fillRect(0, h - barH, w, barH, barColor);

  tft.setTextDatum(top_center);
  tft.setTextSize(1);

  tft.setTextColor(TFT_WHITE, barColor);
  tft.drawString(g_lastTrackName, w / 2, h - barH + 4);

  tft.setTextColor(tft.color565(180, 180, 180), barColor);
  tft.drawString(g_lastArtistName, w / 2, h - barH + 20);
}


// NEW: outer progress ring
static void drawProgressRing() {
  if (g_durationMs == 0) return;

  float ratio = (float)g_progressMs / (float)g_durationMs;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;

  const int16_t cx = 120, cy = 120;
  const int16_t rOuter = 118;
  const int16_t rInner = 112;

  uint16_t bgColor = tft.color565(25, 25, 25);
  uint16_t fgColor = tft.color565(0, 200, 120);

  // Background ring
  tft.drawCircle(cx, cy, rOuter, bgColor);
  tft.drawCircle(cx, cy, rInner, bgColor);

  // Foreground arc
  int segments = 120; // 3° steps
  for (int i = 0; i < segments * ratio; ++i) {
    float ang = (float)i / segments * 2.0f * M_PI - M_PI_2;
    int16_t x0 = cx + cosf(ang) * rInner;
    int16_t y0 = cy + sinf(ang) * rInner;
    int16_t x1 = cx + cosf(ang) * rOuter;
    int16_t y1 = cy + sinf(ang) * rOuter;
    tft.drawLine(x0, y0, x1, y1, fgColor);
  }
}

// ---- Spinning record draw ----
static void drawSpinningRecord() {
    const int16_t cx = 120, cy = 110, r = 100;

    g_spinAngle += 1.5f;
    g_spinAngle = fmodf(g_spinAngle, 360.0f);

    float rad  = g_spinAngle * (M_PI / 180.0f);
    float cosA = cosf(rad), sinA = sinf(rad);

    static uint16_t rowBuf[240];

    tft.startWrite();
    for (int16_t py = -r; py <= r; py++) {
        int16_t halfW = (int16_t)sqrtf((float)(r * r - py * py));
        int16_t x0 = cx - halfW;
        int16_t x1 = cx + halfW;
        int16_t len = x1 - x0 + 1;

        for (int16_t i = 0; i < len; i++) {
            int16_t px = (x0 + i) - cx;

            float srcX = cosA * px + sinA * py + 120.0f;
            float srcY = -sinA * px + cosA * py + 120.0f;

            int sx = (int)srcX, sy = (int)srcY;
            rowBuf[i] = (sx >= 0 && sx < 240 && sy >= 0 && sy < 240)
                        ? g_frameBuf[sy * 240 + sx]
                        : TFT_BLACK;
        }

        tft.setAddrWindow(x0, cy + py, len, 1);
        tft.pushPixels(rowBuf, len);
    }
    tft.endWrite();
}

// ---- WiFi ----
static bool connectWifiWithRetries() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 20; ++i) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    ::delay(500);
  }
  Serial.println("WiFi: connect failed");
  return false;
}

// ---- Spotify token ----
static bool refreshSpotifyToken() {
  if (WiFi.status() != WL_CONNECTED) return false;

  const String basic   = base64Encode(String(SPOTIFY_CLIENT_ID) + ":" + String(SPOTIFY_CLIENT_SECRET));
  const String payload = "grant_type=refresh_token&refresh_token=" + urlEncode(String(SPOTIFY_REFRESH_TOKEN));

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, TOKEN_URL)) {
    Serial.println("Token: HTTP begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", "Basic " + basic);

  const int    code = http.POST(payload);
  const String body = http.getString();
  http.end();

  Serial.printf("Token: HTTP %d\n", code);
  if (code != 200) return false;

  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, body)) return false;

  const char *token      = doc["access_token"];
  const uint32_t expiresIn = doc["expires_in"] | 3600;
  if (!token || token[0] == '\0') return false;

  g_accessToken      = token;
  g_tokenExpiresAtMs = ::millis() + (expiresIn * 1000UL);
  Serial.printf("Token: refreshed, expires in %lu s\n", static_cast<unsigned long>(expiresIn));
  return true;
}

static bool ensureValidToken() {
  if (!g_accessToken.isEmpty()) {
    if (g_tokenExpiresAtMs > ::millis() + 60000UL) return true;
  }
  return refreshSpotifyToken();
}

// ---- Spotify currently-playing ----
static bool fetchCurrentlyPlaying(String &trackId,
                                  String &albumUrl,
                                  bool &hasTrack,
                                  String &trackName,
                                  String &artistName,
                                  uint32_t &progressMs,   // NEW
                                  uint32_t &durationMs) { // NEW
  trackId = ""; albumUrl = ""; hasTrack = false;
  trackName = ""; artistName = "";
  progressMs = 0; durationMs = 0;

  if (WiFi.status() != WL_CONNECTED || g_accessToken.isEmpty()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, CURRENT_URL)) return false;
  http.addHeader("Authorization", "Bearer " + g_accessToken);

  const int code = http.GET();
  if (code == 204) { http.end(); return true; }

  const String body = http.getString();
  http.end();

  Serial.printf("Currently-playing: HTTP %d\n", code);

  if (code == 401) { g_accessToken.clear(); g_tokenExpiresAtMs = 0; return false; }
  if (code != 200) return false;

  StaticJsonDocument<192> filter;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["images"][0]["url"] = true;
  filter["item"]["album"]["images"][1]["url"] = true;
  filter["progress_ms"] = true;                // NEW
  filter["item"]["duration_ms"] = true;        // NEW

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

  const char *id   = doc["item"]["id"]   | "";
  const char *name = doc["item"]["name"] | "";
  const char *art  = doc["item"]["artists"][0]["name"] | "";
  JsonArray images = doc["item"]["album"]["images"].as<JsonArray>();

  uint32_t prog = doc["progress_ms"] | 0;
  uint32_t dur  = doc["item"]["duration_ms"] | 0;

  if (!id || id[0] == '\0' || images.isNull() || images.size() == 0) return true;

  const char *url = nullptr;
  if (images.size() > 1 && !images[1]["url"].isNull()) url = images[1]["url"];
  else if (!images[0]["url"].isNull()) url = images[0]["url"];

  if (!url || url[0] == '\0') return true;

  trackId    = id;
  albumUrl   = url;
  trackName  = name;
  artistName = art;
  progressMs = prog;
  durationMs = dur;
  hasTrack   = true;
  return true;
}

// ---- Image download ----
static bool downloadImageToBuffer(const String &url, uint8_t *buffer,
                                  size_t bufferSize, size_t &imageSize) {
  imageSize = 0;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) return false;

  const int code = http.GET();
  if (code != 200) { http.end(); return false; }

  const int contentLength = http.getSize();
  if (contentLength > 0 && static_cast<size_t>(contentLength) > bufferSize) {
    http.end(); return false;
  }

  WiFiClient *stream  = http.getStreamPtr();
  uint32_t    startMs = ::millis();

  while (http.connected() &&
         (contentLength < 0 || static_cast<int>(imageSize) < contentLength)) {
    const size_t avail = stream->available();
    if (avail == 0) {
      if (::millis() - startMs > HTTP_TIMEOUT_MS) { http.end(); return false; }
      ::delay(2); continue;
    }
    size_t toRead = avail;
    if (contentLength > 0) {
      const size_t remain = static_cast<size_t>(contentLength) - imageSize;
      if (toRead > remain) toRead = remain;
    }
    if (imageSize + toRead > bufferSize) { http.end(); return false; }
    const int read = stream->readBytes(reinterpret_cast<char *>(buffer + imageSize), toRead);
    if (read <= 0) { ::delay(2); continue; }
    imageSize += static_cast<size_t>(read);
    startMs = ::millis();
  }

  http.end();
  if (imageSize == 0) return false;

  Serial.printf("Image: downloaded %u bytes\n", static_cast<unsigned>(imageSize));
  return true;
}
// Spinner globals + forward declarations
static float g_spinnerAngle = 0.0f;
static bool  g_spinnerInit  = false;

static void drawSpinnerFrame();
static void runStartupSpinnerOnce();

// ---- Worker task ----
static void spotifyWorkerTask(void *param) {
  (void)param;

  uint8_t *imageBuffer = (uint8_t *)heap_caps_malloc(MAX_IMAGE_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  g_frameBuf = (uint16_t *)heap_caps_malloc(240 * 240 * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

  uint32_t lastWifiRetry = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
  // animate spinner + text while waiting
  const int16_t cx = 120;
  const int16_t cy = 120;

      drawSpinnerFrame();
    tft.setTextDatum(middle_center);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Connecting WiFi...", cx, cy);   // centered

  if (::millis() - lastWifiRetry >= WIFI_RETRY_DELAY_MS) {
    lastWifiRetry = ::millis();
    connectWifiWithRetries();
  }

  vTaskDelay(pdMS_TO_TICKS(16));  // ~60 FPS spinner
  continue;
}


    if (!ensureValidToken()) {
  Serial.println("Task: token unavailable, will retry");

  const int16_t cx = 120;
  const int16_t cy = 120;

  drawSpinnerFrame();
  tft.setTextDatum(middle_center);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting Spotify...", cx, cy);
  vTaskDelay(pdMS_TO_TICKS(200));
  continue;
}



    String trackId, albumUrl, trackName, artistName;
    bool   hasTrack = false;
    uint32_t progressMs = 0, durationMs = 0;

    if (!fetchCurrentlyPlaying(trackId, albumUrl, hasTrack,
                               trackName, artistName,
                               progressMs, durationMs)) {
      Serial.println("Task: currently-playing fetch failed");
      drawStatusScreen("Spotify error", "Retrying..."); // NEW
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    if (!hasTrack) {
      Serial.println("Task: nothing currently playing");
      drawStatusScreen("Nothing playing", "Open Spotify"); // NEW
      vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
      continue;
    }

    if (trackId == g_lastTrackId && albumUrl == g_lastAlbumUrl) {
  // Same track — just keep spinning without re-downloading
  if (g_durationMs > 0) {
    uint32_t delta = ::millis() - g_lastUpdateMs;
    g_lastUpdateMs += delta;
    uint64_t p = (uint64_t)g_progressMs + delta;
    if (p > g_durationMs) p = g_durationMs;
    g_progressMs = (uint32_t)p;
  }

  uint32_t frameStart = ::millis();
  drawSpinningRecord();
  drawProgressRing();
  drawTrackOverlay();        // <- add this
  esp_task_wdt_reset();
  int32_t elapsed   = (int32_t)(::millis() - frameStart);
  int32_t remaining = 16 - elapsed;
  if (remaining > 0) vTaskDelay(pdMS_TO_TICKS(remaining));
  continue;
}


    size_t imageSize = 0;
    if (!downloadImageToBuffer(albumUrl, imageBuffer, MAX_IMAGE_BYTES, imageSize)) {
      Serial.println("Task: image download failed");
      drawStatusScreen("Image download", "Failed"); // NEW
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    memset(g_frameBuf, 0, 240 * 240 * sizeof(uint16_t));
    g_imageX = (240 - 150) / 2;
    g_imageY = (240 - 150) / 2;
    TJpgDec.setJpgScale(JPG_SCALE);
    JRESULT res = TJpgDec.drawJpg(0, 0, imageBuffer, imageSize);

    if (res != JDR_OK) {
      Serial.printf("Task: JPEG decode failed (%d)\n", (int)res);
      drawStatusScreen("JPEG decode", "Failed"); // NEW
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    g_lastTrackId    = trackId;
    g_lastAlbumUrl   = albumUrl;
    g_lastTrackName  = trackName;
    g_lastArtistName = artistName;
    g_progressMs     = progressMs;
    g_durationMs     = durationMs;
    g_lastUpdateMs   = ::millis();

    Serial.println("Task: decoded OK, spinning...");


    uint32_t lastPoll = ::millis();
    for (;;) {
      // approximate progress
      if (g_durationMs > 0) {
        uint32_t delta = ::millis() - g_lastUpdateMs;
        g_lastUpdateMs += delta;
        uint64_t p = (uint64_t)g_progressMs + delta;
        if (p > g_durationMs) p = g_durationMs;
        g_progressMs = (uint32_t)p;
      }

      uint32_t frameStart = ::millis();
      drawSpinningRecord();
      drawProgressRing();
      drawTrackOverlay();  
      esp_task_wdt_reset();
      int32_t elapsed   = (int32_t)(::millis() - frameStart);
      int32_t remaining = 16 - elapsed;
      if (remaining > 0) vTaskDelay(pdMS_TO_TICKS(remaining));

      if (::millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = ::millis();
        String newTrackId, newAlbumUrl, newTrackName, newArtistName;
        bool   newHasTrack = false;
        uint32_t newProgressMs = 0, newDurationMs = 0;

        if (ensureValidToken() &&
            fetchCurrentlyPlaying(newTrackId, newAlbumUrl, newHasTrack,
                                  newTrackName, newArtistName,
                                  newProgressMs, newDurationMs) &&
            newHasTrack &&
            (newTrackId != g_lastTrackId || newAlbumUrl != g_lastAlbumUrl)) {
          break;
        }
      }
    }
  }
}



static void drawSpinnerFrame()
{
  const int16_t cx = 120;
  const int16_t cy = 120;
  const int16_t rOuter = 90;
  const int16_t rInner = 70;

  uint16_t bgColor  = TFT_BLACK;
  uint16_t ringCol  = tft.color565(0x1D, 0xB9, 0x54);
  uint16_t textCol  = tft.color565(220, 220, 220);

  if (!g_spinnerInit) {
    tft.fillScreen(bgColor);
    tft.setTextDatum(middle_center);
    tft.setTextSize(1);
    tft.setTextColor(textCol, bgColor);
    tft.drawString("Spotify Player", cx, cy + 60);
    g_spinnerInit = true;
  }

  // Clear logo area only
  tft.fillCircle(cx, cy, rOuter + 4, bgColor);

  // Static outer ring
  tft.drawCircle(cx, cy, rOuter, ringCol);
  tft.drawCircle(cx, cy, rInner, ringCol);

  // Spinning "S" arc
  for (int i = 0; i < 3; ++i) {
    float a0 = g_spinnerAngle + i * 18.0f;
    float a1 = a0 + 30.0f;

    for (float a = a0; a <= a1; a += 2.0f) {
      float rad  = a * (M_PI / 180.0f);
      int16_t x0 = cx + cosf(rad) * rInner;
      int16_t y0 = cy + sinf(rad) * rInner;
      int16_t x1 = cx + cosf(rad) * rOuter;
      int16_t y1 = cy + sinf(rad) * rOuter;
      tft.drawLine(x0, y0, x1, y1, ringCol);
    }
  }

  g_spinnerAngle += 8.0f;
  if (g_spinnerAngle >= 360.0f) g_spinnerAngle -= 360.0f;
}

static void runStartupSpinnerOnce()
{
  g_spinnerInit = false;
  uint32_t start = ::millis();
  while (::millis() - start < 1000) {   // ~1 second
    drawSpinnerFrame();
    ::delay(16);
  }
  // Leave last spinner frame on screen; no clear here
}



// ---- Arduino setup/loop ----
void setup() {
  Serial.begin(115200);
  ::delay(200);

  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);

  TJpgDec.setCallback(tftJpgOutput);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setJpgScale(JPG_SCALE);

  // NEW: run spinner for a second at boot
  runStartupSpinnerOnce();

  xTaskCreatePinnedToCore(
      spotifyWorkerTask,
      "spotify_worker",
      12288,
      nullptr,
      1,
      nullptr,
      1);
}

void loop() {
  ::delay(1000);
}
