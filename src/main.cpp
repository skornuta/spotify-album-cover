#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <TJpg_Decoder.h>
#include "secrets.h"

using namespace lgfx;

// ---- Pins / constants ----
static constexpr int PIN_SCLK = 18;
static constexpr int PIN_MOSI = 23;
static constexpr int PIN_MISO = -1;
static constexpr int PIN_DC   = 2;
static constexpr int PIN_CS   = 5;
static constexpr int PIN_RST  = 4;


static constexpr uint32_t SPI_WRITE_FREQ = 20000000;
static constexpr uint32_t SPI_READ_FREQ  = 8000000;

static constexpr uint32_t POLL_INTERVAL_MS      = 15000;
static constexpr uint32_t WIFI_RETRY_DELAY_MS   = 10000;
static constexpr uint32_t HTTP_TIMEOUT_MS       = 15000;
static constexpr size_t MAX_IMAGE_BYTES = 65536;  // 64 KB is enough for a 300x300 JPEG
static constexpr uint8_t JPG_SCALE = 1;           // Keep scale=1; the JPEG file itself is ~20–40 KB

static const char *TOKEN_URL   = "https://accounts.spotify.com/api/token";
static const char *CURRENT_URL = "https://api.spotify.com/v1/me/player/currently-playing";
static constexpr bool DISPLAY_SELF_TEST = true;

// ---- Board/Spotify secrets checks ----
#ifndef WIFI_SSID
#error "WIFI_SSID is missing. Create include/secrets.h from include/secrets.example.h"
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS is missing. Create include/secrets.h from include/secrets.example.h"
#endif
#ifndef SPOTIFY_CLIENT_ID
#error "SPOTIFY_CLIENT_ID is missing. Create include/secrets.h from include/secrets.example.h"
#endif
#ifndef SPOTIFY_CLIENT_SECRET
#error "SPOTIFY_CLIENT_SECRET is missing. Create include/secrets.h from include/secrets.example.h"
#endif
#ifndef SPOTIFY_REFRESH_TOKEN
#error "SPOTIFY_REFRESH_TOKEN is missing. Create include/secrets.h from include/secrets.example.h"
#endif

// ---- LovyanGFX display ----
class LGFX : public LGFX_Device {
 public:
  LGFX() {
    { // SPI bus
      auto cfg = _bus.config();
      cfg.spi_host   = VSPI_HOST;   // ESP32-D VSPI
      cfg.spi_mode   = 0;
      cfg.freq_write = SPI_WRITE_FREQ;
      cfg.freq_read  = SPI_READ_FREQ;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk   = PIN_SCLK;
      cfg.pin_mosi   = PIN_MOSI;
      cfg.pin_miso   = PIN_MISO;    // -1 -> no MISO
      cfg.pin_dc     = PIN_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // GC9A01 panel
      auto cfg = _panel.config();
      cfg.pin_cs         = PIN_CS;
      cfg.pin_rst        = PIN_RST;
      cfg.pin_busy       = -1;
      cfg.memory_width   = 240;
      cfg.memory_height  = 240;
      cfg.panel_width    = 240;
      cfg.panel_height   = 240;
      cfg.offset_x       = 0;
      cfg.offset_y       = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 0;
      cfg.dummy_read_bits  = 0;
      cfg.readable       = false;   // write-only
      cfg.invert         = false;
      cfg.rgb_order      = true;
      cfg.dlen_16bit     = false;
      cfg.bus_shared     = false;
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
static int16_t g_imageX = 0;
static int16_t g_imageY = 0;

static String   g_accessToken;
static uint32_t g_tokenExpiresAtMs = 0;
static String   g_lastAlbumUrl;
static String   g_lastTrackId;

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
    const bool safe = (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') ||
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

// ---- TJpg_Decoder output callback ----
static bool tftJpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) {
    return false;
  }
  tft.pushImage(g_imageX + x, g_imageY + y, w, h, bitmap);
  return true;
}

// ---- WiFi + Spotify token ----
static bool connectWifiWithRetries() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

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

static bool refreshSpotifyToken() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

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
  Serial.println("Token payload:");
  Serial.println(body);

  if (code != 200) {
    return false;
  }

  StaticJsonDocument<768> doc;
  const auto err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Token JSON parse error: %s\n", err.c_str());
    return false;
  }

  const char   *token     = doc["access_token"];
  const uint32_t expiresIn = doc["expires_in"] | 3600;
  if (!token || token[0] == '\0') {
    Serial.println("Token JSON missing access_token");
    return false;
  }

  g_accessToken       = token;
  g_tokenExpiresAtMs  = ::millis() + (expiresIn * 1000UL);
  Serial.printf("Token: refreshed, expires in %lu s\n",
                static_cast<unsigned long>(expiresIn));
  return true;
}

static bool ensureValidToken() {
  if (!g_accessToken.isEmpty()) {
    const uint32_t now = ::millis();
    if (g_tokenExpiresAtMs > now + 60000UL) {
      return true;
    }
  }
  return refreshSpotifyToken();
}

// ---- Spotify currently-playing ----
static bool fetchCurrentlyPlaying(String &trackId, String &albumUrl, bool &hasTrack) {
  trackId  = "";
  albumUrl = "";
  hasTrack = false;

  if (WiFi.status() != WL_CONNECTED || g_accessToken.isEmpty()) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, CURRENT_URL)) {
    Serial.println("Currently-playing: HTTP begin failed");
    return false;
  }

  http.addHeader("Authorization", "Bearer " + g_accessToken);

  const int code = http.GET();
  if (code == 204) {
    http.end();
    Serial.println("Currently-playing: 204 no content");
    return true;
  }

  const String body = http.getString();
  http.end();

  Serial.printf("Currently-playing: HTTP %d\n", code);
  Serial.println("Currently-playing payload:");
  Serial.println(body);

  if (code == 401) {
    g_accessToken.clear();
    g_tokenExpiresAtMs = 0;
    return false;
  }
  if (code != 200) {
    return false;
  }

  // Filter: id + first two album images URLs
  StaticJsonDocument<192> filter;
  filter["item"]["id"] = true;
  filter["item"]["album"]["images"][0]["url"] = true;
  filter["item"]["album"]["images"][1]["url"] = true;

  StaticJsonDocument<2048> doc;
  const auto err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("Currently-playing JSON parse error: %s\n", err.c_str());
    return false;
  }

  const char *id = doc["item"]["id"] | "";
  JsonArray   images = doc["item"]["album"]["images"].as<JsonArray>();

  if (!id || id[0] == '\0' || images.isNull() || images.size() == 0) {
    Serial.println("Currently-playing: no playable item or album images");
    return true;
  }

  const char *url = nullptr;
  if (images.size() > 1 && !images[1]["url"].isNull()) {
    url = images[1]["url"];  // 300x300 preferred
  } else if (!images[0]["url"].isNull()) {
    url = images[0]["url"];
  }

  if (!url || url[0] == '\0') {
    Serial.println("Currently-playing: album image URL missing");
    return true;
  }

  trackId  = id;
  albumUrl = url;
  hasTrack = true;
  return true;
}

// ---- Image download ----
static bool downloadImageToBuffer(const String &url, uint8_t *buffer,
                                  size_t bufferSize, size_t &imageSize) {
  imageSize = 0;

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("Image: HTTP begin failed");
    return false;
  }

  const int code = http.GET();
  if (code != 200) {
    Serial.printf("Image: HTTP %d\n", code);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength > 0 &&
      static_cast<size_t>(contentLength) > bufferSize) {
    Serial.printf("Image: too large (%d > %u bytes)\n",
                  contentLength, static_cast<unsigned>(bufferSize));
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint32_t    startMs = ::millis();

  while (http.connected() &&
         (contentLength < 0 || static_cast<int>(imageSize) < contentLength)) {
    const size_t avail = stream->available();
    if (avail == 0) {
      if (::millis() - startMs > HTTP_TIMEOUT_MS) {
        Serial.println("Image: stream timeout");
        http.end();
        return false;
      }
      ::delay(2);
      continue;
    }

    size_t toRead = avail;
    if (contentLength > 0) {
      const size_t remain = static_cast<size_t>(contentLength) - imageSize;
      if (toRead > remain) {
        toRead = remain;
      }
    }

    if (imageSize + toRead > bufferSize) {
      Serial.printf("Image: exceeded MAX_IMAGE_BYTES (%u)\n",
                    static_cast<unsigned>(bufferSize));
      http.end();
      return false;
    }

    const int read = stream->readBytes(
        reinterpret_cast<char *>(buffer + imageSize), toRead);
    if (read <= 0) {
      ::delay(2);
      continue;
    }

    imageSize += static_cast<size_t>(read);
    startMs = ::millis();
  }

  http.end();

  if (imageSize == 0) {
    Serial.println("Image: empty download");
    return false;
  }

  Serial.printf("Image: downloaded %u bytes\n",
                static_cast<unsigned>(imageSize));
  return true;
}

// ---- JPEG decode + draw ----
static bool decodeAndDrawCentered(const uint8_t *jpgData, size_t jpgSize) {
  uint16_t jpgW = 0, jpgH = 0;
  if (!TJpgDec.getJpgSize(&jpgW, &jpgH, jpgData, jpgSize)) {
    Serial.println("JPEG: failed to read dimensions");
    return false;
  }

  uint16_t drawW = jpgW / JPG_SCALE;
  uint16_t drawH = jpgH / JPG_SCALE;
  if (!drawW) drawW = 1;
  if (!drawH) drawH = 1;

  g_imageX = static_cast<int16_t>((tft.width()  - drawW) / 2);
  g_imageY = static_cast<int16_t>((tft.height() - drawH) / 2);

  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(JPG_SCALE);

  tft.startWrite();
  JRESULT res = TJpgDec.drawJpg(0, 0, jpgData, jpgSize);
  tft.endWrite();

  Serial.printf("JPEG: decode result=%d (srcW=%u srcH=%u scale=%u)\n",
                (int)res, jpgW, jpgH, JPG_SCALE);

  return res == JDR_OK;
}

// ---- Worker task ----
static void spotifyWorkerTask(void *param) {
  (void)param;

  uint8_t *imageBuffer = (uint8_t *)heap_caps_malloc(MAX_IMAGE_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!imageBuffer) {
    Serial.println("Task: failed to allocate image buffer");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  uint32_t lastWifiRetry = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (::millis() - lastWifiRetry >= WIFI_RETRY_DELAY_MS) {
        lastWifiRetry = ::millis();
        connectWifiWithRetries();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!ensureValidToken()) {
      Serial.println("Task: token unavailable, will retry");
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    String trackId;
    String albumUrl;
    bool   hasTrack = false;

    if (!fetchCurrentlyPlaying(trackId, albumUrl, hasTrack)) {
      Serial.println("Task: currently-playing fetch failed");
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    if (!hasTrack) {
      Serial.println("Task: nothing currently playing");
      vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
      continue;
    }

    if (trackId == g_lastTrackId && albumUrl == g_lastAlbumUrl) {
      Serial.println("Task: same track/art, skipping redraw");
      vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
      continue;
    }

    size_t imageSize = 0;
    if (!downloadImageToBuffer(albumUrl, imageBuffer,
                               MAX_IMAGE_BYTES, imageSize)) {
      Serial.println("Task: image download failed");
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    if (!decodeAndDrawCentered(imageBuffer, imageSize)) {
      Serial.println("Task: JPEG decode failed");
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    g_lastTrackId  = trackId;
    g_lastAlbumUrl = albumUrl;

    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
  }
}

// ---- Arduino setup/loop ----
void setup() {
  Serial.begin(115200);
  ::delay(200);

  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(middle_center);
  tft.drawString("Starting...", tft.width() / 2, tft.height() / 2);

  TJpgDec.setCallback(tftJpgOutput);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setJpgScale(JPG_SCALE);

  if (DISPLAY_SELF_TEST) {
    tft.fillScreen(TFT_RED);
    ::delay(400);
    tft.fillScreen(TFT_GREEN);
    ::delay(400);
    tft.fillScreen(TFT_BLUE);
    ::delay(400);
    tft.fillScreen(TFT_BLACK);
    tft.drawString("SPI OK?", tft.width() / 2, tft.height() / 2);
    ::delay(800);
  }

  xTaskCreatePinnedToCore(
      spotifyWorkerTask,
      "spotify_worker",
      12288,     // stack words (~32 KB)
      nullptr,
      1,
      nullptr,
      0);       // core 0 on ESP32
}   // <-- this closing brace must be here, alone on this line

void loop() {
  ::delay(1000);
}
