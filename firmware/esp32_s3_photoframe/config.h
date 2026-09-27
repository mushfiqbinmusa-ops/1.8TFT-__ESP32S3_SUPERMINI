// =============================================================================
//  config.h  --  ALL tunables in one place.
//
//  Nothing in this file is a secret. WiFi credentials, the server address and
//  the device token live in secrets.h instead, so that file stays out of git.
//
//  The real firmware includes secrets.h. To build from this repo:
//
//      cp secrets.h.example secrets.h
//      $EDITOR secrets.h
//
// =============================================================================

#ifndef PHOTOFRAME_CONFIG_H
#define PHOTOFRAME_CONFIG_H

// -----------------------------------------------------------------------------
//  IDENTITY
//
//  Reported to the server in GET /api/system-info. The server writes these onto
//  the device row the first time it reaches the frame, which is what makes the
//  web UI show a proper name and the correct 160x128 dimensions instead of the
//  800x480 placeholder it falls back to for an unreachable device.
// -----------------------------------------------------------------------------
#define DEVICE_NAME        "SuperMini 1.8 TFT"
#define BOARD_NAME         "esp32s3_st7735"
#define FIRMWARE_VERSION   "1.0.0"

//  Colour model of the panel.
//
//  This is the value the server keys its dithering decision off, via
//  Device.IsGrayscale() at backend/internal/model/model.go:126:
//
//      return strings.HasPrefix(d.DisplayType, "gc") || d.BoardName == "seeedstudio_reterminal_e1003"
//
//  So only the "gc" prefix matters. "spectra6" and "" both mean "not grayscale",
//  and the server treats them identically. For a colour TFT, report "spectra6" --
//  it is the honest description, and it is the value that keeps the grayscale
//  ramp out of the pipeline.
//
//  !! DO NOT SET THIS TO "gc16" TO "FIX" A LOOKING-WRONG IMAGE. !!
//  "gc16" forces a 16-level grayscale ramp on a panel that can display 65,536
//  colours. It is only a step up from 6-colour dithering, never a fix, and it
//  throws away colour permanently. If photos come back looking wrong with
//  "spectra6" set, the cause is the server's pipeline, not this string.
//
//  Verified: backend/internal/handler/image.go:445-493 sends EVERY photo through
//  epaper-image-convert, including when the firmware sends no X-Color-Palette
//  and no X-Processing-Settings. With no palette argument the converter still
//  runs, so this TFT does not automatically get truecolour. That needs the
//  server-side bypass described in firmware/README.md section 6, which the
//  firmware requests by reporting full_colour below.
// -----------------------------------------------------------------------------
#define DISPLAY_TYPE       "spectra6"

//  Tells the server this panel can display truecolour, so it skips the
//  e-paper dithering pipeline and sends a normal PNG. The server honours
//  this only to turn the bypass ON -- a frame that omits the field (or
//  reports false) never switches it back off, because the operator has to be
//  able to turn it off in the web UI.
//
//  Set to false only if you swap the panel for something that genuinely
//  cannot do colour.
// -----------------------------------------------------------------------------
#define REPORT_FULL_COLOUR true

// -----------------------------------------------------------------------------
//  PINS
//
//  Fixed by the wiring table. Do not change without rewiring.
//    GPIO 0  BOOT button + strapping pin -- keep free
//    GPIO 3  strapping pin
//    GPIO 4  UNUSED -- the backlight moved off it, see BACKLIGHT_ON_GPIO4
//    GPIO 5  TFT RESET
//    GPIO 6  TFT A0/DC
//    GPIO 10 TFT CS
//    GPIO 11 TFT SDA/MOSI  (FSPI)
//    GPIO 12 TFT SCK       (FSPI)
//    GPIO 19/20 native USB -- keep free
//    GPIO 43/44 UART0 -- keep free
//    GPIO 48 onboard WS2812 -- keep free
// -----------------------------------------------------------------------------
#define PIN_TFT_CS      10
#define PIN_TFT_RST      5
#define PIN_TFT_DC       6
#define PIN_TFT_MOSI    11
#define PIN_TFT_SCLK    12

//  Where the backlight is wired, and therefore whether the firmware may touch
//  it at all.
//
//  false = the panel's BLK/LED pin goes straight to the 3.3V rail, and the
//          firmware must NEVER drive GPIO 4. It also does not need to: the
//          backlight is on as soon as the module is powered.
//  true  = the panel's BLK/LED pin goes to GPIO 4 and the firmware drives it.
//
//  !! This is false because a GPIO cannot power this backlight. !!
//  An ESP32-S3 pin sources roughly 8-12 mA reliably (20 mA absolute maximum).
//  The 1.8" panel's backlight wants 20-40 mA. Driving BLK from GPIO 4 therefore
//  works "sort of": the panel lights, but so faintly that you have to look for
//  it. That is a current limit, not a logic fault, and no firmware change can
//  fix it -- the 3.3V pin is a regulator output and can supply the LED properly.
//
//  SAFETY: with the backlight on 3.3V, GPIO 4 must be left disconnected from the
//  panel. If it stayed wired and anything ever drove that pin LOW, it would short
//  the 3.3V rail to ground through the module.
//
//  Switching this back to true requires rewiring BLK to GPIO 4, and accepting
//  the dim backlight. It also requires a series resistor on the panel's
//  backlight if the module does not already have one, so the LED survives being
//  driven from a pin at all.
#define BACKLIGHT_ON_GPIO4  false

//  Only meaningful when BACKLIGHT_ON_GPIO4 is true. Unused otherwise, kept so
//  the wiring table above stays complete.
#define PIN_TFT_LED      4

// -----------------------------------------------------------------------------
//  PANEL GEOMETRY
// -----------------------------------------------------------------------------
#define SCREEN_WIDTH     160   // landscape
#define SCREEN_HEIGHT    128
#define ORIENTATION      "landscape"   // "landscape" | "portrait"

//  1 or 3 = landscape 160x128, 0 or 2 = portrait 128x160.
//  If the image is rotated 90 degrees, change this and nothing else.
#define TFT_ROTATION     1

//  Panel init variant. The 1.8" V1.1 exists in several glass revisions and this
//  is the usual cause of a 1-3 pixel offset with stray coloured lines on one
//  edge. 1: it is offset, try REDTAB, then GREENTAB.
//  2: it is fine.
//  See the Troubleshooting table in ../../README.md.
//    INITR_BLACKTAB / INITR_REDTAB / INITR_GREENTAB
#define TFT_TAB          INITR_BLACKTAB

//  Some V1.1 modules ship wired for inverted colours (white renders black).
//  Set to true if Gate 1 reported a negative image.
#define TFT_INVERT       false

//  The ST7735 consumes RGB565 big-endian, which is what PNGdec emits by
//  default. Set to false if reds and blues come out swapped.
#define PNG_BIG_ENDIAN   true

// -----------------------------------------------------------------------------
//  SYNC BEHAVIOUR
// -----------------------------------------------------------------------------

//  How often to pull a new photo. 15 minutes is the sensible default: every
//  fetch costs the server a full render plus an epaper-image-convert subprocess,
//  and the server applies no rate limit at all, so this is a politeness budget
//  rather than a technical limit.
#define REFRESH_INTERVAL_MS   (15UL * 60UL * 1000UL)

//  How soon to try again after a FAILED fetch.
//
//  Separate from REFRESH_INTERVAL_MS on purpose. A failed fetch usually means
//  something transient -- the AP dropped us, the Pi was rebooting -- and by the
//  time the full refresh interval expires the frame has been showing a stale
//  photo for a quarter of an hour, which is indistinguishable from a dead
//  frame. One minute recovers quickly without hammering the server, which does
//  a full render per request and applies no rate limit.
#define SYNC_RETRY_AFTER_FAIL_MS (60UL * 1000UL)

//  HTTP timeout. The guide's blueprint uses 10 s, which is too short: the
//  server renders synchronously (headless Chrome for overlays, then the
//  converter subprocess) before it emits the first byte, and sets no server-side
//  timeouts at all. The server's own outbound client to a frame allows 120 s.
//  45 s leaves room for a cold render without hanging the display for two
//  minutes.
#define HTTP_TIMEOUT_MS      45000

//  Attempts per sync cycle before giving up and leaving the cached photo alone.
#define HTTP_RETRY_COUNT     2

//  Base delay for the exponential backoff between retries.
#define HTTP_RETRY_BACKOFF_MS 2000

//  Wi-Fi association timeout. The guide warns against blocking indefinitely;
//  on failure the firmware falls back to the flash cache.
#define WIFI_CONNECT_TIMEOUT_MS 20000

//  On boot, open a bare TCP connection to the gateway and to the server before
//  the first real fetch, and log how long each took.
//
//  This exists because the interesting failures are otherwise indistinguishable.
//  "HTTP -1" from HTTPClient means connect() failed; "HTTP -11" means the TCP
//  connection came up and then no response byte ever arrived. Both look
//  identical on the glass, and telling them apart is the difference between
//  "the server is down" and "the frame cannot reach the server's subnet".
//  Costs about 100 ms when the network is healthy.
#define NET_DIAG_ON_BOOT   true

// -----------------------------------------------------------------------------
//  FLASH CACHE
//
//  Relative to the 4 MB of on-module flash. 160x128 PNGs land around 5-35 KB,
//  so this is generous. LittleFS (not the deprecated SPIFFS).
// -----------------------------------------------------------------------------
#define CACHE_PATH      "/current_photo.png"
#define CACHE_TEMP_PATH "/current_photo.tmp"

//  Refuse anything larger before allocating it. Guards against a runaway
//  response filling the heap.
#define MAX_IMAGE_BYTES (512 * 1024)

//  Hard cap on the header count we will read back off a response, so a
//  misbehaving server cannot make us allocate without bound.
#define MAX_RESPONSE_HEADERS 24

// -----------------------------------------------------------------------------
//  OPTIONAL FEATURES
// -----------------------------------------------------------------------------

//  Serve GET /api/system-info and GET /api/config on the frame itself.
//
//  /api/system-info is what lets the web UI show the real name and dimensions
//  and makes its "Open Device" and "Refresh" buttons work; without it the
//  server just falls back to 800x480 and the buttons error out.
//
//  /api/config is the other half of the server's push mechanism. Leave this on
//  if DISPLAY_TYPE is unset to something the server would try to probe; see
//  README.md before disabling.
//
#define SERVE_FRAME_API   true

//  Port the frame's own API listens on.
#define FRAME_API_PORT    80

//  Accept and apply the X-Config-Payload response header, so the web UI can
//  drive refresh interval, sleep schedule, timezone and NTP server.
//  Requires SERVE_FRAME_API, because the server answers a device-side config
//  push by polling the frame's /api/config.
#define ENABLE_CONFIG_SYNC true

//  Send X-Battery-Percentage. Only meaningful on a build with a battery gauge
//  attached; harmless (and visible in the web UI's device list) otherwise.
#define SEND_BATTERY_HEADER true
#define BATTERY_PERCENT    100

//  Feed the task watchdog from loop(). The Wi-Fi and HTTP calls are blocking,
//  so this must be larger than the worst-case HTTP timeout plus retries.
#define WDT_TIMEOUT_S      90

#endif  // PHOTOFRAME_CONFIG_H
