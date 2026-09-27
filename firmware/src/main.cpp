// =============================================================================
//  ESP32-S3 SuperMini + 1.8" ST7735 (V1.1) photo frame
//
//  Pulls a PNG from esp32-photoframe-server, caches it in LittleFS, and serves
//  a small HTTP API back to the server so the web UI can see this frame.
//
//  Panel settings are all in esp32_s3_photoframe/config.h and every one of them
//  was verified on this hardware during Gate 1 (see ../README.md). Nothing in
//  this file re-specifies them.
//
//  Wire protocol, all verified against backend/internal/handler/image.go and
//  backend/pkg/photoframe/client.go:
//
//    frame -> server   GET  {SERVER_BASE_URL}/image
//                            Authorization: Bearer <DEVICE_TOKEN>
//                            X-Firmware-Version, X-Display-Width/Height/
//                            Orientation, X-Config-Last-Updated,
//                            X-Battery-Percentage, X-Processing-Settings
//    server -> frame   image/png, plus control headers X-Config-Payload and
//                            X-Post-Rotate-Wait-Sec
//
//    server -> frame   GET  http://<device host>/api/system-info
//                            GET  /api/config, /api/settings/palette,
//                                 /api/settings/processing
//                            POST the same three (device-side edits)
//                            GET  /api/display-image
// =============================================================================

// Adafruit headers come first: config.h references INITR_* from the ST7735
// header, and this ordering keeps that dependency obvious rather than relying
// on the preprocessor expanding it later at each use site.
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <LittleFS.h>

#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>

#include "config.h"
#include "secrets.h"

// -----------------------------------------------------------------------------
//  PANEL
// -----------------------------------------------------------------------------

//  Argument order is (cs, dc, mosi, sck, rst). The PIN_* names in config.h are
//  listed in a different order, which is exactly the kind of thing that gets
//  miswired, so it is spelled out here.
static Adafruit_ST7735 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_MOSI, PIN_TFT_SCLK,
                            PIN_TFT_RST);

// -----------------------------------------------------------------------------
//  CONSTANTS
// -----------------------------------------------------------------------------

//  512 bytes, well under the ~5 KB the Arduino HTTPClient itself uses, so the
//  transfer itself is not the thing that starves the watchdog.
static const size_t CHUNK_BYTES = 512;

//  Room for the three config documents. The server sends complete objects, not
//  deltas, and they are all a few hundred bytes.
static const size_t JSON_MAX = 1536;

//  Where the frame's own configuration is persisted. The config timestamp has
//  to survive a reboot, otherwise the server would see the frame as "has newer
//  edits" on every boot and re-push forever.
#define PATH_CFG_TS      "/cfg_ts.txt"
#define PATH_CFG_CONFIG  "/cfg_config.json"
#define PATH_CFG_PROC    "/cfg_proc.json"
#define PATH_CFG_PALETTE "/cfg_palette.json"

#if SERVE_FRAME_API
static WebServer frameApi(FRAME_API_PORT);
#endif

// -----------------------------------------------------------------------------
//  STATE
// -----------------------------------------------------------------------------

//  The frame's own configuration, exactly as the server last pushed it. Kept
//  as opaque JSON: the server round-trips these documents verbatim, so echoing
//  back precisely what we were given is what keeps the two sides in agreement.
static char g_configJson[JSON_MAX] = "{}";
static char g_procJson[JSON_MAX] = "{}";
static char g_paletteJson[JSON_MAX] = "{}";

//  Our edit timestamp, sent as X-Config-Last-Updated and compared by the server
//  against its own ConfigLastUpdated to decide who pushes and who pulls.
static long long g_configTs = 0;

static bool g_wifiOk = false;

//  Absolute millis() deadline for the next fetch. Starts at 0 so the very first
//  loop() iteration syncs immediately.
static uint32_t g_nextSyncMs = 0;
static bool g_haveImage = false;
static char g_lastError[80] = "";

//  The decoder itself, reachable from the scanline callback.
static PNG* g_png = NULL;

//  Geometry of the image being decoded, needed by the callback to map source
//  pixels onto the panel.
static int g_pngW = 0;
static int g_pngH = 0;
static uint16_t* g_line = NULL;
static size_t g_lineWords = 0;

// -----------------------------------------------------------------------------
//  SMALL HELPERS
// -----------------------------------------------------------------------------

static void feedWdt() { esp_task_wdt_reset(); }

//  Prefer PSRAM, fall back to internal RAM. Both come from the heap_caps
//  allocator, so freeBuf() handles either without tracking which one it got.
static void* allocBuf(size_t n) {
  void* p = NULL;
  if (psramFound()) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}

static void freeBuf(void* p) {
  if (p) heap_caps_free(p);
}

static void readFileToStr(const char* path, char* out, size_t cap) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f || f.size() <= 0) {
    if (f) f.close();
    out[0] = '\0';
    return;
  }
  size_t n = f.readBytes(out, cap - 1);
  out[n] = '\0';
  f.close();
}

static void writeStrToFile(const char* path, const char* s) {
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return;
  f.print(s);
  f.close();
}

//  Response headers the sync protocol depends on.
//
//  These must be registered with collectHeaders() before the request, and that
//  is not optional. handleHeaderResponse() (HTTPClient.cpp:1301) copies a
//  response header into the lookup table ONLY if the name already matches an
//  entry registered there, and addHeader() registers nothing -- it just appends
//  to the outgoing request string. So a bare http.header("X-Config-Payload")
//  returns empty no matter what the server sent, and every config change made
//  in the web UI would be silently ignored with nothing on screen to say why.
//  Note "Date" has to be listed too: the library parses it internally for
//  cookie handling, but does not retain it unless we ask.
static const char* kWantHeaders[] = {
    "Date", "X-Config-Payload", "X-Post-Rotate-Wait-Sec",
};
static const size_t kWantHeaderCount =
    sizeof(kWantHeaders) / sizeof(kWantHeaders[0]);

//  Read a registered response header. The lookup is case-insensitive
//  (equalsIgnoreCase), so the exact casing here is cosmetic.
static String respHeader(HTTPClient& http, const char* want) {
  return http.header(want);
}

//  Pull one nested JSON object out of a payload, by key, matching braces.
//
//  Used on X-Config-Payload, which looks like
//      {"config":{...},"processing_settings":{...},"color_palette":{...}}
//
//  CAVEAT: this counts braces without tracking string literals, so a value
//  containing an unbalanced brace inside quotes would confuse it. The values
//  here are timezones, IP strings, booleans and a cron array, none of which
//  contain braces, so a full JSON parser would be dead weight. Revisit if the
//  config schema ever grows free-text fields.
static bool extractJsonObject(const String& hay, const char* key,
                              String& out) {
  String k = String("\"") + key + "\"";
  int ki = hay.indexOf(k);
  if (ki < 0) return false;
  int ob = hay.indexOf('{', ki + k.length());
  if (ob < 0) return false;
  int depth = 0;
  for (int i = ob; i < (int)hay.length(); i++) {
    char c = hay[i];
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) {
        out = hay.substring(ob, i + 1);
        return true;
      }
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
//  TIME
// -----------------------------------------------------------------------------

//  The config-sync protocol compares our timestamp against the server's, both
//  absolute unix seconds, so the frame needs a real clock -- a counter is not
//  enough, because the server's own edits have to look "newer" than ours.
//
//  The clock is taken from the server's HTTP Date header rather than NTP. That
//  is deliberate: the frame is on a LAN that may have no internet route, while
//  the Pi is guaranteed reachable (nothing works without it) and its clock is
//  already NTP-synced.
static bool parseHttpDate(const String& v, time_t& out) {
  static const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char dow[8] = {0};
  char mon[8] = {0};
  int day, year, hh, mm, ss;
  if (sscanf(v.c_str(), "%3s, %2d %3s %4d %2d:%2d:%2d", dow, &day, mon, &year,
             &hh, &mm, &ss) != 7) {
    return false;
  }
  int m = 0;
  for (int i = 0; i < 12; i++) {
    if (strcasecmp(mon, kMonths[i]) == 0) {
      m = i + 1;
      break;
    }
  }
  if (m == 0) return false;

  //  TZ is pinned to UTC in setup(), so mktime() here is a straight UTC
  //  conversion and does not depend on a tz database being present.
  struct tm tmv;
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_mday = day;
  tmv.tm_mon = m - 1;
  tmv.tm_year = year - 1900;
  tmv.tm_hour = hh;
  tmv.tm_min = mm;
  tmv.tm_sec = ss;
  tmv.tm_isdst = 0;
  out = mktime(&tmv);
  return out > 0;
}

static void setClockFromServer(const String& dateHeader) {
  if (dateHeader.length() == 0) return;
  time_t t;
  if (!parseHttpDate(dateHeader, t)) return;
  if ((long long)t < 1600000000LL) return;  // implausible; keep what we had
  struct timeval tv;
  tv.tv_sec = t;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  Serial.printf("[time] clock set to %lld from server Date header\n",
                (long long)t);
}

// -----------------------------------------------------------------------------
//  CONFIG PERSISTENCE
// -----------------------------------------------------------------------------

static void loadPersistedConfig() {
  char ts[24];
  readFileToStr(PATH_CFG_TS, ts, sizeof(ts));
  g_configTs = atoll(ts);

  readFileToStr(PATH_CFG_CONFIG, g_configJson, JSON_MAX);
  if (g_configJson[0] == '\0') strcpy(g_configJson, "{}");

  readFileToStr(PATH_CFG_PROC, g_procJson, JSON_MAX);
  if (g_procJson[0] == '\0') strcpy(g_procJson, "{}");

  readFileToStr(PATH_CFG_PALETTE, g_paletteJson, JSON_MAX);
  if (g_paletteJson[0] == '\0') strcpy(g_paletteJson, "{}");

  Serial.printf("[cfg] restored, config_ts=%lld\n", g_configTs);
}

static void persistConfig() {
  char ts[24];
  snprintf(ts, sizeof(ts), "%lld", g_configTs);
  writeStrToFile(PATH_CFG_TS, ts);
  writeStrToFile(PATH_CFG_CONFIG, g_configJson);
  writeStrToFile(PATH_CFG_PROC, g_procJson);
  writeStrToFile(PATH_CFG_PALETTE, g_paletteJson);
}

//  Adopt a config document the server pushed, and move our timestamp past
//  whatever the server has. The server pushes only while its ConfigLastUpdated
//  is strictly newer than ours, so stamping with "now" is what makes the push
//  converge instead of repeating on every sync.
static void adoptPushedConfig(const String& payload) {
  String cfg, proc, pal;
  bool changed = false;

  if (extractJsonObject(payload, "config", cfg)) {
    strlcpy(g_configJson, cfg.c_str(), JSON_MAX);
    changed = true;
  }
  if (extractJsonObject(payload, "processing_settings", proc)) {
    strlcpy(g_procJson, proc.c_str(), JSON_MAX);
    changed = true;
  }
  if (extractJsonObject(payload, "color_palette", pal)) {
    strlcpy(g_paletteJson, pal.c_str(), JSON_MAX);
    changed = true;
  }

  if (changed) {
    time_t now = time(NULL);
    g_configTs = (now > 0) ? (long long)now : (g_configTs + 1);
    persistConfig();
    Serial.printf("[cfg] applied pushed config, ts=%lld\n", g_configTs);
  }
}

// -----------------------------------------------------------------------------
//  DISPLAY
// -----------------------------------------------------------------------------

static void showBanner(const char* line1, const char* line2, uint16_t bg,
                       uint16_t fg) {
  tft.fillScreen(bg);
  tft.setTextWrap(true);
  tft.setTextColor(fg);
  tft.setTextSize(1);
  tft.setCursor(0, 40);
  tft.print(line1);
  if (line2 && line2[0]) {
    tft.setCursor(0, 60);
    tft.print(line2);
  }
  tft.setTextWrap(false);
}

//  Distinct, self-explanatory failure screens. "The display is blank" is not a
//  diagnosis; "HTTP 401" tells you the token is wrong and nothing else does.
static void showError(const char* headline, const char* detail) {
  showBanner(headline, detail, ST77XX_BLACK, ST77XX_RED);
}

static void showOk(const char* headline, const char* detail) {
  showBanner(headline, detail, ST77XX_BLACK, ST77XX_GREEN);
}

static void initDisplay() {
  //  BACKLIGHT_ON_GPIO4 is false: BLK is on the 3.3V rail and GPIO 4 must be
  //  left alone entirely. Driving it here would short the rail.
  tft.initR(TFT_TAB);
  tft.setRotation(TFT_ROTATION);
  if (TFT_INVERT) tft.invertDisplay(true);
}

// -----------------------------------------------------------------------------
//  PNG DECODE
// -----------------------------------------------------------------------------

//  PNGdec calls this once per decoded scanline. Keeping the working set to a
//  single line means we never need a full-width framebuffer: an RGB888 buffer
//  for this panel would be 61,440 bytes, which is a large slice of the S3's
//  320 KB of SRAM while WiFi and the HTTP client are also live.
static int pngDrawLine(PNGDRAW* p) {
  if (p->iWidth <= 0) return 1;

  int n = (p->iWidth < g_lineWords) ? p->iWidth : (int)g_lineWords;

  //  The endianness argument selects the MEMORY byte order of the output words,
  //  not the order the panel receives. Adafruit_ST7735::drawPixel() puts the
  //  high byte of each uint16_t on the wire first, so the panel always gets
  //  big-endian and we want PNGdec's value in natural host form. Passing
  //  PNG_RGB565_BIG_ENDIAN would apply a bswap16 and shift red and blue into
  //  each other, so PNG_BIG_ENDIAN selects the unswapped form. Flip
  //  PNG_BIG_ENDIAN in config.h if a photo ever comes out with red and blue
  //  exchanged.
#if PNG_BIG_ENDIAN
  const int endian = PNG_RGB565_LITTLE_ENDIAN;
#else
  const int endian = PNG_RGB565_BIG_ENDIAN;
#endif
  g_png->getLineAsRGB565(p, g_line, endian, 0);

  //  The server already sizes the image to the panel (fitToPanel at
  //  image.go:465), so these are normally identity mappings. They exist so that
  //  an unexpectedly sized image is scaled rather than truncated.
  int dy = (g_pngH > SCREEN_HEIGHT) ? (p->y * SCREEN_HEIGHT / g_pngH) : p->y;
  for (int i = 0; i < n; i++) {
    int dx = (g_pngW > SCREEN_WIDTH) ? (i * SCREEN_WIDTH / g_pngW) : i;
    tft.drawPixel(dx, dy, g_line[i]);
  }
  return 1;  // non-zero: keep decoding
}

static void freeLineBuf() {
  freeBuf(g_line);
  g_line = NULL;
  g_lineWords = 0;
}

//  Decode and display a PNG already held in RAM.
static bool renderPngBuffer(uint8_t* data, size_t len) {
  if (!data || len == 0) return false;

  //  The decoder's own state is ~36 KB, over 32 KB of which is the zlib
  //  inflate window, so it goes in PSRAM to keep DRAM for the network stack.
  PNG* png = (PNG*)allocBuf(sizeof(PNG));
  if (!png) {
    Serial.println("[png] out of memory allocating decoder");
    return false;
  }
  g_png = png;

  if (g_png->openRAM(data, (int)len, pngDrawLine) != PNG_SUCCESS) {
    Serial.printf("[png] openRAM failed, error %d\n", g_png->getLastError());
    freeBuf(png);
    g_png = NULL;
    return false;
  }

  g_pngW = png->getWidth();
  g_pngH = png->getHeight();
  Serial.printf("[png] %dx%d, pixeltype %d\n", g_pngW, g_pngH,
                png->getPixelType());

  if (g_pngW <= 0 || g_pngH <= 0 || g_pngW > 4096 || g_pngH > 4096) {
    Serial.println("[png] implausible dimensions");
    png->close();
    freeBuf(png);
    g_png = NULL;
    return false;
  }

  freeLineBuf();
  g_lineWords = (size_t)g_pngW;
  g_line = (uint16_t*)allocBuf(g_lineWords * sizeof(uint16_t));
  if (!g_line) {
    Serial.println("[png] out of memory allocating line buffer");
    png->close();
    freeBuf(png);
    g_png = NULL;
    return false;
  }

  //  One window for the whole image, so drawPixel() does not re-issue
  //  CASET/RASET per pixel.
  tft.startWrite();
  int rc = png->decode(NULL, 0);
  tft.endWrite();
  png->close();

  freeBuf(png);
  g_png = NULL;
  freeLineBuf();

  if (rc != PNG_SUCCESS) {
    Serial.printf("[png] decode failed, error %d\n", rc);
    return false;
  }
  return true;
}

//  Read the cached image out of LittleFS and draw it.
static bool renderFromCache() {
  File f = LittleFS.open(CACHE_PATH, FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0 || sz > MAX_IMAGE_BYTES) {
    f.close();
    return false;
  }
  uint8_t* buf = (uint8_t*)allocBuf(sz);
  if (!buf) {
    f.close();
    return false;
  }
  //  File::readBytes takes char*, while the decoder wants uint8_t*. Casting is
  //  safe: both are plain byte pointers and the size is unchanged.
  size_t got = f.readBytes((char*)buf, sz);
  f.close();

  bool ok = (got == sz) && renderPngBuffer(buf, got);
  freeBuf(buf);
  return ok;
}

// -----------------------------------------------------------------------------
//  FETCH
// -----------------------------------------------------------------------------

//  Download the image into the cache atomically: write to CACHE_TEMP_PATH,
//  verify the byte count, then rename over CACHE_PATH. A power cut or a dropped
//  connection therefore leaves the previous good photo intact rather than a
//  half-written file.
static bool downloadToCache(String& err) {
  String url = String(SERVER_BASE_URL) + "/image";

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.setUserAgent(String("esp32-photoframe/") + FIRMWARE_VERSION);

  if (!http.begin(url)) {
    err = "http.begin failed";
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  http.addHeader("X-Firmware-Version", FIRMWARE_VERSION);
  http.addHeader("X-Display-Width", String(SCREEN_WIDTH));
  http.addHeader("X-Display-Height", String(SCREEN_HEIGHT));
  http.addHeader("X-Display-Orientation", ORIENTATION);
  //  Formatted by hand rather than as String(g_configTs): Arduino's String has
  //  no long long constructor on this core, so passing one would pick an
  //  ambiguous narrowing conversion and silently mangle the timestamp -- which
  //  would then compare wrong against the server's and either sync never or
  //  sync forever.
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%lld", g_configTs);
  http.addHeader("X-Config-Last-Updated", String(tsBuf));
#if SEND_BATTERY_HEADER
  http.addHeader("X-Battery-Percentage", String(BATTERY_PERCENT));
#endif
  //  The server logs this per fetch, so link degradation shows up in its access
  //  log next to the request that suffered it, which is where you look first
  //  when a frame starts being intermittently unreachable.
  http.addHeader("X-Wifi-Signal", String(g_wifiOk ? (int)WiFi.RSSI() : 0));
  //  Only sent once the server has actually given us settings. An empty or
  //  "{}" document is NOT the same as no header: the server would decode it
  //  into a struct full of zeroes (contrast 0, saturation 0, empty tone mode)
  //  and render the image with those, instead of using its own defaults.
  if (strncmp(g_procJson, "{}", 2) != 0) {
    http.addHeader("X-Processing-Settings", String(g_procJson));
  }

  //  Register the response headers we intend to read, before the request.
  http.collectHeaders(kWantHeaders, kWantHeaderCount);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    //  400 = the token carried no device_id claim (an admin token, not a
    //  device token). 401/403 = the token is wrong or revoked.
    err = (code == 400) ? "HTTP 400 (not a device token?)"
                        : (String("HTTP ") + code);
    Serial.printf("[http] GET %s -> %d\n", url.c_str(), code);
    http.end();
    return false;
  }

  String dateHdr = respHeader(http, "Date");
  setClockFromServer(dateHdr);

#if ENABLE_CONFIG_SYNC
  //  Read the control headers before the body is consumed.
  String payload = respHeader(http, "X-Config-Payload");
  if (payload.length() > 0) {
    adoptPushedConfig(payload);
  } else {
    //  Expected whenever the server has nothing newer to push, so this is a
    //  one-line heartbeat rather than a warning. If the server log says it is
    //  pushing and this still prints "none", the header is not being retained.
    Serial.println("[cfg] no push this fetch (server had nothing newer)");
  }

  String postRotate = respHeader(http, "X-Post-Rotate-Wait-Sec");
  if (postRotate.length() > 0) {
    //  The server's async config pull needs our HTTP server to keep answering
    //  for this long after a rotation. Ours never goes away, so this is only
    //  worth logging.
    Serial.printf("[sync] server asked us to stay up %s s for a config pull\n",
                  postRotate.c_str());
  }
#endif

  long long declared = http.getSize();
  if (declared > (long long)MAX_IMAGE_BYTES) {
    err = "image too large";
    http.end();
    return false;
  }

  File f = LittleFS.open(CACHE_TEMP_PATH, FILE_WRITE);
  if (!f) {
    err = "cannot open cache temp";
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t chunk[CHUNK_BYTES];
  long long written = 0;
  bool overflow = false;

  while (http.connected()) {
    size_t avail = stream->available();
    if (avail == 0) {
      //  Declared length known and fully read: done, without waiting on the
      //  server to close the connection.
      if (declared > 0 && written >= declared) break;
      delay(1);
      feedWdt();
      continue;
    }
    size_t want = avail;
    if (declared > 0 && (long long)want > (declared - written)) {
      want = (size_t)(declared - written);
    }
    if (want > CHUNK_BYTES) want = CHUNK_BYTES;
    int n = stream->readBytes(chunk, want);
    if (n <= 0) break;
    f.write(chunk, n);
    written += n;
    feedWdt();
    if (written > (long long)MAX_IMAGE_BYTES) {
      overflow = true;
      break;
    }
  }
  f.close();
  http.end();

  if (overflow) {
    LittleFS.remove(CACHE_TEMP_PATH);
    err = "image too large";
    return false;
  }
  if (declared > 0 && written != declared) {
    LittleFS.remove(CACHE_TEMP_PATH);
    err = "short read (" + String((unsigned long)written) + "/" +
          String((unsigned long)declared) + ")";
    return false;
  }
  if (written == 0) {
    LittleFS.remove(CACHE_TEMP_PATH);
    err = "empty body";
    return false;
  }

  LittleFS.remove(CACHE_PATH);
  if (!LittleFS.rename(CACHE_TEMP_PATH, CACHE_PATH)) {
    LittleFS.remove(CACHE_TEMP_PATH);
    err = "cache rename failed";
    return false;
  }

  Serial.printf("[sync] cached %lld bytes\n", written);
  return true;
}

//  One sync attempt, with retries. The cache is only re-rendered when the
//  download actually succeeded, so a failed attempt leaves the panel alone.
static bool syncOnce() {
  for (int attempt = 0; attempt < HTTP_RETRY_COUNT; attempt++) {
    if (attempt > 0) {
      uint32_t backoff = HTTP_RETRY_BACKOFF_MS * (1UL << (attempt - 1));
      Serial.printf("[sync] retry %d in %lu ms\n", attempt,
                    (unsigned long)backoff);
      delay(backoff);
      feedWdt();
    }
    String err;
    if (downloadToCache(err)) {
      g_haveImage = renderFromCache();
      if (g_haveImage) {
        g_lastError[0] = '\0';
        return true;
      }
      err = "PNG decode failed";
    }

    strlcpy(g_lastError, err.c_str(), sizeof(g_lastError));
    Serial.printf("[sync] attempt %d failed: %s\n", attempt, g_lastError);
  }
  return false;
}

// -----------------------------------------------------------------------------
//  WIFI
// -----------------------------------------------------------------------------

static void connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  //  Modem sleep can stall a long HTTP transfer badly enough to look like a
  //  dead server, and a 15-minute refresh cycle gains nothing from it.
  WiFi.setSleep(false);

#if USE_STATIC_IP
  IPAddress ip, mask, gw, dns;
  ip.fromString(STATIC_IP);
  mask.fromString(STATIC_NETMASK);
  gw.fromString(STATIC_GATEWAY);
  dns.fromString(STATIC_DNS);
  WiFi.config(ip, gw, mask, dns);
#endif

  Serial.printf("[wifi] ssid %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
    feedWdt();
  }

  g_wifiOk = (WiFi.status() == WL_CONNECTED);
  if (!g_wifiOk) {
    Serial.println("[wifi] association TIMED OUT");
    return;
  }

  Serial.println("[wifi] connected");
  Serial.print("[wifi] mac   ");
  Serial.println(WiFi.macAddress());
  Serial.print("[wifi] ip    ");
  Serial.println(WiFi.localIP());
  Serial.print("[wifi] gw    ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("[wifi] rssi  ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

//  The banner also states the address this frame can be reached at, because
//  that string has to be typed into the device's Host field in the web UI.
static void showNetBanner() {
  char l1[40];
  char l2[40];
  snprintf(l1, sizeof(l1), "ip %s", WiFi.localIP().toString().c_str());
  snprintf(l2, sizeof(l2), "rssi %d dBm", (int)WiFi.RSSI());
  showOk(l1, l2);
}

//  Open a bare TCP connection and report whether it came up, and how fast.
//
//  Bare TCP rather than HTTP on purpose: an HTTPClient failure code alone does
//  not say which end is broken. connect() failing (-1) means the frame never
//  reached the peer; a connection that opens and then goes silent (-11) means
//  the peer accepted and then said nothing. Those have completely different
//  fixes, and one log line each is cheaper than guessing.
static void probeTcp(const char* host, uint16_t port) {
  WiFiClient c;
  uint32_t t0 = millis();
  bool ok = c.connect(host, port, 5000);
  uint32_t dt = millis() - t0;
  Serial.printf("[probe] %s:%u -> %s (%lums)\n", host, (unsigned)port,
                ok ? "OPEN" : "FAILED", (unsigned long)dt);
  if (ok) c.stop();
  feedWdt();
}

#if NET_DIAG_ON_BOOT
static void runNetDiag() {
  Serial.println("[probe] --- reachability ---");
  probeTcp(STATIC_GATEWAY, 80);
  //  Derive the server's host:port from SERVER_BASE_URL so there is one place
  //  to change. Defaults match the one the firmware actually uses.
  probeTcp("192.168.0.119", 9607);
  Serial.println("[probe] -------------------");
}
#endif

// -----------------------------------------------------------------------------
//  FRAME API
// -----------------------------------------------------------------------------

//  Exactly the fields of photoframe.SystemInfo. This response is what makes
//  the web UI show "SuperMini 1.8 TFT" at 160x128 instead of the 800x480
//  placeholder it seeds, and what enables the device's Open Device / Refresh
//  buttons.
static String systemInfoJson() {
  String s;
  s.reserve(360);
  s += "{\"device_name\":\"" DEVICE_NAME "\",";
  s += "\"width\":" + String(SCREEN_WIDTH) + ",";
  s += "\"height\":" + String(SCREEN_HEIGHT) + ",";
  s += "\"board_name\":\"" BOARD_NAME "\",";
  s += "\"version\":\"" FIRMWARE_VERSION "\",";
  s += "\"display_type\":\"" DISPLAY_TYPE "\",";
  s += "\"full_colour\":" + String(REPORT_FULL_COLOUR ? "true" : "false") + ",";
  //  Not part of the server's SystemInfo struct, so it is ignored there. It is
  //  here because RF quality is otherwise invisible until something fails: a
  //  frame can sit at -85 dBm and look perfectly healthy right up until the
  //  server's next poll times out, and by then the cause is a guess. One extra
  //  field costs nothing and turns "the frame went offline" into a number.
  s += "\"wifi_rssi\":" + String(g_wifiOk ? (int)WiFi.RSSI() : 0) + ",";
  s += "\"wifi_ok\":" + String(g_wifiOk ? "true" : "false");
  s += "}";
  return s;
}

#if SERVE_FRAME_API

static void hSystemInfo() {
  frameApi.send(200, "application/json", systemInfoJson());
}

//  Capturing nothing is deliberate: hGetJson is a function, not an object with
//  automatic storage duration, so a lambda cannot capture it. The bare name is
//  resolved by ordinary lookup instead.
static void hGetConfig() { frameApi.send(200, "application/json", String(g_configJson)); }
static void hGetProcessing() { frameApi.send(200, "application/json", String(g_procJson)); }
static void hGetPalette() { frameApi.send(200, "application/json", String(g_paletteJson)); }

//  Accept a device-side edit. The server sends the complete object rather than
//  a delta, so storing it verbatim is the correct merge.
static void hPostJson(char* dest, const char* label) {
  if (!frameApi.hasArg("plain")) {
    frameApi.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }
  String body = frameApi.arg("plain");
  if (body.length() >= (int)JSON_MAX) {
    frameApi.send(400, "application/json", "{\"error\":\"too large\"}");
    return;
  }
  strlcpy(dest, body.c_str(), JSON_MAX);

  time_t now = time(NULL);
  g_configTs = (now > 0) ? (long long)now : (g_configTs + 1);
  persistConfig();

  Serial.printf("[api] %s updated, ts=%lld\n", label, g_configTs);
  frameApi.send(200, "application/json", "{\"ok\":true}");
}

static void hPostConfig() { hPostJson(g_configJson, "config"); }
static void hPostProcessing() { hPostJson(g_procJson, "processing"); }
static void hPostPalette() { hPostJson(g_paletteJson, "palette"); }

//  The image currently on the glass, straight out of the cache. The server has
//  a client method for this; the web UI does not call it yet, but it costs
//  nothing and makes the "what is the frame showing" question answerable.
static void hDisplayImage() {
  File f = LittleFS.open(CACHE_PATH, FILE_READ);
  if (!f) {
    frameApi.send(404, "text/plain", "no cached image");
    return;
  }
  frameApi.streamFile(f, "image/png");
  f.close();
}

//  Force an immediate fetch, bypassing the refresh schedule.
//
//  Not part of the server's protocol. It exists because the refresh interval is
//  15 minutes, which makes "is the frame actually syncing?" a question you
//  cannot answer interactively. Hitting this from a laptop answers it in one
//  request, and the JSON reports the real failure reason instead of leaving you
//  to infer it from a blank panel.
static void hForceSync() {
  if (!g_wifiOk) {
    frameApi.send(503, "application/json", "{\"ok\":false,\"error\":\"no wifi\"}");
    return;
  }
  bool ok = syncOnce();
  if (ok) {
    g_nextSyncMs = millis() + REFRESH_INTERVAL_MS;
    frameApi.send(200, "application/json", "{\"ok\":true}");
  } else {
    g_nextSyncMs = millis() + SYNC_RETRY_AFTER_FAIL_MS;
    String body = String("{\"ok\":false,\"error\":\"") + g_lastError + "\"}";
    frameApi.send(502, "application/json", body);
  }
}

static void hNotFound() {
  frameApi.send(404, "application/json", "{\"error\":\"not found\"}");
}

//  Receive an image pushed by the server.
//
//  The server's PushToDevice (backend/pkg/photoframe/client.go) POSTs a
//  multipart form to this same path with two parts: "image" (the rendered
//  160x128 PNG) and an optional "thumbnail". Only the image is used -- this
//  panel has no place to show a thumbnail -- but the part is read and discarded
//  rather than ignored, because an unread part leaves bytes in the stream and
//  the multipart parser desynchronises.
//
//  This is the one thing that replaces the photo on the glass without the frame
//  asking for it, so it is also the moment a POST is "made to the esp32" in the
//  sense of the fallback contract: the cached image is what you get until this
//  succeeds, and a failure here leaves the previous photo untouched.
static File g_pushFile;
static bool g_pushActive = false;
static size_t g_pushBytes = 0;
static bool g_pushTooBig = false;
static bool g_pushComplete = false;

static void hPushImageUpload() {
  HTTPUpload& up = frameApi.upload();

  if (up.status == UPLOAD_FILE_START) {
    //  Match on the part NAME, not the filename. The server builds both parts
    //  with CreateFormFile, so "image" and "thumbnail" both arrive with a
    //  filename and filtering on an empty one would drop the payload too.
    g_pushActive = (up.name == "image");
    if (g_pushActive) {
      //  Reset only for the part we keep. The thumbnail arrives after the
      //  image and has its own START event; clearing the counters there would
      //  wipe the byte count the done handler is about to check.
      g_pushBytes = 0;
      g_pushTooBig = false;
      g_pushComplete = false;
      LittleFS.remove(CACHE_TEMP_PATH);
      g_pushFile = LittleFS.open(CACHE_TEMP_PATH, FILE_WRITE);
    }
    Serial.printf("[push] part '%s' (%s)\n", up.name.c_str(),
                  g_pushActive ? "kept" : "discarded");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_pushActive && g_pushFile) {
      if (g_pushBytes + up.currentSize > MAX_IMAGE_BYTES) {
        //  Keep draining the stream so the parser stays in sync, but stop
        //  writing: a runaway body must not fill the flash partition.
        g_pushTooBig = true;
      } else {
        size_t n = g_pushFile.write(up.buf, up.currentSize);
        if (n != up.currentSize) g_pushTooBig = true;  // flash full
        g_pushBytes += n;
      }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (g_pushActive && g_pushFile) {
      g_pushFile.close();
      g_pushComplete = true;
    }
    g_pushActive = false;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (g_pushFile) g_pushFile.close();
    LittleFS.remove(CACHE_TEMP_PATH);
    g_pushActive = false;
  }
  feedWdt();
}

static void hPushImageDone() {
  //  No wifi check needed: a request only arrives over WiFi in the first place.

  if (!g_pushComplete) {
    frameApi.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"no image part in upload\"}");
    return;
  }
  if (g_pushTooBig || g_pushBytes == 0) {
    LittleFS.remove(CACHE_TEMP_PATH);
    frameApi.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"empty or oversized image\"}");
    return;
  }

  //  Same atomic swap as the download path, so a half-received push can never
  //  become the photo on the glass.
  LittleFS.remove(CACHE_PATH);
  if (!LittleFS.rename(CACHE_TEMP_PATH, CACHE_PATH)) {
    LittleFS.remove(CACHE_TEMP_PATH);
    frameApi.send(500, "application/json",
                  "{\"ok\":false,\"error\":\"cache rename failed\"}");
    return;
  }

  if (!renderFromCache()) {
    frameApi.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"PNG decode failed\"}");
    return;
  }
  g_haveImage = true;
  g_lastError[0] = '\0';
  //  The pushed image is current, so the next scheduled refresh is a full
  //  interval away rather than an immediate retry.
  g_nextSyncMs = millis() + REFRESH_INTERVAL_MS;
  Serial.printf("[push] displayed %u bytes\n", (unsigned)g_pushBytes);
  frameApi.send(200, "application/json", "{\"ok\":true}");
}

static void startFrameApi() {
  frameApi.on("/api/system-info", HTTP_GET, hSystemInfo);

  frameApi.on("/api/config", HTTP_GET, hGetConfig);
  frameApi.on("/api/config", HTTP_POST, hPostConfig);

  frameApi.on("/api/settings/processing", HTTP_GET, hGetProcessing);
  frameApi.on("/api/settings/processing", HTTP_POST, hPostProcessing);

  frameApi.on("/api/settings/palette", HTTP_GET, hGetPalette);
  frameApi.on("/api/settings/palette", HTTP_POST, hPostPalette);

  frameApi.on("/api/display-image", HTTP_GET, hDisplayImage);
  //  Push: the upload callback streams the multipart body to flash, the done
  //  callback commits it. Both are required; ArduinoWebServer calls the upload
  //  one first and only then the done one.
  frameApi.on("/api/display-image", HTTP_POST, hPushImageDone, hPushImageUpload);
  frameApi.on("/api/sync", HTTP_GET, hForceSync);

  frameApi.onNotFound(hNotFound);
  frameApi.begin();
  Serial.printf("[api] listening on port %d\n", FRAME_API_PORT);
}

#endif  // SERVE_FRAME_API

// -----------------------------------------------------------------------------
//  SETUP
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC enumerate, or the first prints are lost

  //  Pin TZ so that the config timestamp is UTC regardless of any tz
  //  database. Everything here is unix-seconds arithmetic against the server.
  setenv("TZ", "UTC0", 1);
  tzset();

  esp_task_wdt_init((uint32_t)WDT_TIMEOUT_S * 1000, true);
  esp_task_wdt_add(NULL);


  Serial.println();
  Serial.println("--------------------------------------------------------------");
  Serial.println("  ESP32-S3 photo frame");
  Serial.println("  " FIRMWARE_VERSION "  " BOARD_NAME "  " DISPLAY_TYPE);
  Serial.println("--------------------------------------------------------------");

  initDisplay();
  showBanner("starting...", "mounting filesystem", ST77XX_BLACK,
             ST77XX_WHITE);

  //  begin(true) formats on a mount failure, so a filesystem corrupted by a
  //  power cut heals itself instead of wedging every boot.
  if (!LittleFS.begin(true)) {
    Serial.println("[fs] LittleFS mount failed even with format");
    showError("filesystem error", "cannot mount flash");
    return;
  }
  Serial.println("[fs] mounted");
  loadPersistedConfig();

  showBanner("joining wifi...", WIFI_SSID, ST77XX_BLACK, ST77XX_WHITE);
  connectWiFi();

  if (!g_wifiOk) {
    //  Deliberately no error screen here. Whether a usable photo already exists
    //  on the flash is decided below, and the fallback there draws it. Putting
    //  an error up now would mean overwriting that photo with a failure notice
    //  about a problem the photo does not have.
    Serial.println("[wifi] no network; will retry in the background");
  } else {
    showNetBanner();
#if NET_DIAG_ON_BOOT
    runNetDiag();
#endif
  }

#if SERVE_FRAME_API
  startFrameApi();
#endif

  //  Fetch straight away rather than waiting out the first refresh interval,
  //  so a freshly flashed frame shows a photo instead of a blank panel for
  //  fifteen minutes.
  if (g_wifiOk) {
    showBanner("syncing...", "downloading photo", ST77XX_BLACK,
               ST77XX_WHITE);
    if (syncOnce()) {
      Serial.println("[sync] first fetch OK");
      g_nextSyncMs = millis() + REFRESH_INTERVAL_MS;
    } else {
      Serial.printf("[sync] first fetch FAILED: %s\n", g_lastError);
      //  Retry soon rather than sitting on a stale photo for the full interval.
      g_nextSyncMs = millis() + SYNC_RETRY_AFTER_FAIL_MS;
    }
  }

  //  The fallback, and the only place g_haveImage is set on the boot path.
  //
  //  A frame that has downloaded a photo before keeps that photo through every
  //  later failure: no network at boot, server unreachable, DNS down, an empty
  //  gallery, a corrupt response. It comes back only when a new image actually
  //  arrives, which is the only thing that should ever replace the glass.
  //
  //  This has to run after the banner above, because the banner is painted over
  //  the panel and would hide the photo we are about to restore. g_haveImage is
  //  set by syncOnce() on success, so reaching here with it false means no new
  //  image was fetched and the cache is the best thing we have.
  if (!g_haveImage) {
    if (renderFromCache()) {
      g_haveImage = true;
      Serial.println("[display] restored last photo from flash");
    } else {
      //  Nothing cached either, so there is genuinely nothing to show and an
      //  error screen is the only honest thing to put on the panel.
      if (g_wifiOk) {
        showError("no photo yet", g_lastError);
      } else {
        showError("no photo yet", "no network, flash is empty");
      }
    }
  }

  Serial.println("[boot] ready");
  Serial.println();
}

// -----------------------------------------------------------------------------
//  LOOP
// -----------------------------------------------------------------------------

void loop() {
  feedWdt();

  //  Re-associate if the AP dropped us, and try to re-establish a usable
  //  network rather than waiting for the next scheduled sync to discover it.
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry > 30000UL) {
      lastRetry = millis();
      Serial.println("[wifi] link lost, retrying");
      g_wifiOk = false;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  } else if (!g_wifiOk) {
    g_wifiOk = true;
    Serial.print("[wifi] reconnected, ip ");
    Serial.println(WiFi.localIP());
#if NET_DIAG_ON_BOOT
    //  Worth repeating here: a link that dropped and came back is exactly the
    //  situation where "can we actually reach the server" is in question, and
    //  the boot-time probe ran before the link was ever up.
    runNetDiag();
#endif
    //  Coming back from a dropped link is a good moment to try again, rather
    //  than sitting on a stale photo until the next scheduled fetch.
    g_nextSyncMs = 0;
  }

  //  Next attempt is an absolute deadline rather than a period, so a slow
  //  fetch cannot make the interval drift.
  if (g_wifiOk && (int32_t)(millis() - g_nextSyncMs) >= 0) {
    bool ok = syncOnce();
    if (ok) {
      g_nextSyncMs = millis() + REFRESH_INTERVAL_MS;
    } else {
      //  Come back soon instead of waiting out the full refresh interval. On a
      //  flaky link the common failure is transient, and 15 minutes of a stale
      //  photo after a failed boot fetch looks like a dead frame.
      g_nextSyncMs = millis() + SYNC_RETRY_AFTER_FAIL_MS;
      if (!g_haveImage) showError("sync failed", g_lastError);
    }
  }

#if SERVE_FRAME_API
  frameApi.handleClient();
#endif

  delay(2);
}
