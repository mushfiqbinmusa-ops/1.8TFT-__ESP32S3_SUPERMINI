# AI Agent Specification: ESP32-S3 SuperMini & ST7735 PhotoFrame Firmware

> **Instructions for the AI Agent**:  
> You are tasked with generating, modifying, or debugging firmware for an **ESP32-S3 SuperMini** connected to a **1.8-inch TFT SPI color display (Model V1.1, ST7735R/ST7735S driver, 128×160 resolution)**.  
> Follow this document strictly. It contains architectural decisions, library choices, hardware pin constraints, what to do, what to avoid, and complete reference implementations.

---

## 1. System Requirements & Hardware Scope

| Parameter | Specification |
|:---|:---|
| **Microcontroller** | ESP32-S3 SuperMini (ESP32-S3FH4R2: Dual-Core Xtensa LX7, 4MB Flash, 2MB QSPI PSRAM) |
| **Display Panel** | 1.8" TFT SPI Color LCD Module (Model V1.1) |
| **Display Controller** | Sitronix ST7735 / ST7735R / ST7735S |
| **Display Dimensions** | 128 × 160 pixels (Default orientation: Landscape 160 × 128) |
| **Display Color Depth** | 16-bit RGB565 |
| **Logic Level** | 3.3V (Direct compatibility, no level shifters needed) |
| **Backend Server** | `esp32-photoframe-server` (Running on port 9607) |
| **Serving Format** | Standard PNG (`image/png`) delivered via HTTP GET `/image` |

---

## 2. Hardware Pinout Mapping & Wiring Constraints

The user is wiring the hardware themselves. You **must strictly adhere** to this pin mapping.

### Primary Pin Assignment Table

| Display Pin (1.8" TFT V1.1) | ESP32-S3 SuperMini Pin | Function Description | Notes |
|:---|:---|:---|:---|
| **VCC** | **3.3V** (or **5V**) | Power Supply | 3.3V recommended; 5V supported via onboard regulator |
| **GND** | **GND** | System Ground | Must share common ground |
| **CS** | **GPIO 10** | SPI Chip Select | Active LOW |
| **RESET / RES** | **GPIO 5** | Hardware Reset | Active LOW |
| **A0 / DC** | **GPIO 6** | Data / Command Control | LOW = Command, HIGH = Data |
| **SDA / MOSI** | **GPIO 11** | Hardware SPI MOSI | Native FSPI Data Out |
| **SCK / SCL** | **GPIO 12** | Hardware SPI Clock | Native FSPI Clock |
| **LED / BLK** | **GPIO 4** (or **3.3V**) | Display Backlight | High for ON; supports PWM dimming |

### ⚠️ Restricted System Pins (DO NOT USE for Display)
- **GPIO 0**: Boot strapping pin and BOOT button. Must remain free.
- **GPIO 3**: Strapping pin on ESP32-S3; can cause boot failures if pulled inappropriately.
- **GPIO 19 & 20**: Dedicated to native USB CDC / JTAG. Never reassign.
- **GPIO 43 & 44**: Default UART0 (TX/RX) serial pins.
- **GPIO 48**: Connected to onboard WS2812 RGB LED and Red Power LED.

---

## 3. Library Selection Matrix & Technical Rationale

| Functionality | Recommended Library | Version | Technical Rationale |
|:---|:---|:---|:---|
| **TFT Driver** | **`Adafruit_ST7735`** + **`Adafruit_GFX`** | Latest | Enables software definition of pins directly in the `.ino` sketch without requiring external header file modifications (`User_Setup.h`). Supports native ST7735 tab initialization variants (`INITR_BLACKTAB`, `INITR_REDTAB`, `INITR_GREENTAB`). |
| **Alternative Driver** | **`TFT_eSPI`** | Latest | Use only if high-speed DMA or custom rendering pipelines are specifically requested. Requires configuring `User_Setup.h`. |
| **Image Decoder** | **`PNGdec`** (by Larry Bank) | Latest | The server converts photos to standard PNG (`image/png`). `PNGdec` decodes stream data line-by-line using under 4KB of RAM, eliminating memory overflow on ESP32. |
| **Flash Storage** | **`LittleFS`** | Built-in | Persists downloaded images in the 4MB flash so the display reloads instantly when powered on, even if the Raspberry Pi server is offline. (Do NOT use deprecated `SPIFFS`). |
| **Networking** | **`WiFi.h`** + **`HTTPClient.h`** | Built-in | Standard ESP32 core libraries. |

---

## 4. Agent Rules: What to DO and What NOT to DO

### Mandatory DO's:
1. **DO enable USB CDC On Boot**: The ESP32-S3 SuperMini has native USB without a CH340 chip. In Arduino IDE, `Tools -> USB CDC On Boot: Enabled` must be documented; in PlatformIO, `-DARDUINO_USB_CDC_ON_BOOT=1` must be set in `build_flags`.
2. **DO implement offline flash caching with `LittleFS`**: Always save `/photo.png` to internal flash after downloading. On boot, load the cached image first before attempting Wi-Fi sync so the frame shows an image immediately even if the server is unreachable.
3. **DO supply mandatory HTTP request headers to `esp32-photoframe-server`**:
   - `Authorization: Bearer <DEVICE_TOKEN>`
   - `X-Display-Width: 160` (or `128`)
   - `X-Display-Height: 128` (or `160`)
   - `X-Display-Orientation: landscape` (or `portrait`)
   - `X-Firmware-Version: 1.0.0` *(CRITICAL: Omission or reporting a version >= 2.6.1 may cause the server to return EPDGZ e-paper format instead of PNG!)*
4. **DO handle ST7735 V1.1 color and offset variations**:
   - If borders show 1-2 pixel offsets: Switch from `INITR_BLACKTAB` to `INITR_REDTAB`.
   - If colors are inverted (negative image): Call `tft.invertDisplay(true)`.
   - If red and blue are swapped: Change `PNGdec` mode between `PNG_RGB565_BIG_ENDIAN` and `PNG_RGB565_LITTLE_ENDIAN`.
5. **DO use non-blocking timers**: Use `millis()` or ESP32 deep sleep instead of long blocking `delay()` calls.

### Strict DON'Ts:
1. **DON'T allocate huge RAM buffers**: Do NOT allocate full 160×128×2 (40KB) framebuffers multiple times on the heap without checking for `nullptr`.
2. **DON'T block indefinitely on Wi-Fi connect**: Implement a timeout (e.g. 15–20 seconds). If Wi-Fi fails or server is down, display the cached image from `LittleFS` gracefully.
3. **DON'T use `SPIFFS`**: SPIFFS is deprecated in modern ESP32 Arduino Core v3+. Use `LittleFS`.
4. **DON'T omit the Backlight Pin control**: If `GPIO 4` is wired to `LED/BLK`, set `pinMode(4, OUTPUT); digitalWrite(4, HIGH);` or the screen remains dark.

---

## 5. Complete Blueprint 1: Client for `esp32-photoframe-server` (With Offline Caching)

This sketch connects to `esp32-photoframe-server`, downloads the processed PNG, stores it in `LittleFS`, and renders it to the ST7735 display. If the server is offline, it loads the saved image from flash.

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <PNGdec.h>

// =========================================================================
//                           USER CONFIGURATION
// =========================================================================
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";

// Server endpoint (Server Port is 9607)
const char* SERVER_IMAGE_URL = "http://192.168.1.100:9607/image";

// Device Bearer Token generated in Settings -> Security
const char* DEVICE_TOKEN     = "YOUR_DEVICE_TOKEN_HERE";

// Refresh interval (default: 15 minutes)
const unsigned long REFRESH_INTERVAL_MS = 15 * 60 * 1000;

// =========================================================================
//                           PIN DEFINITIONS
// =========================================================================
#define TFT_CS    10
#define TFT_RST    5
#define TFT_DC     6
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_LED    4

#define SCREEN_WIDTH   160
#define SCREEN_HEIGHT  128
#define ORIENTATION    "landscape"

// Objects
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
PNG png;
const char* CACHE_PATH = "/current_photo.png";

// =========================================================================
//                        PNG RENDERING CALLBACK
// =========================================================================
void pngDrawCallback(PNGDRAW *pDraw) {
  uint16_t lineBuffer[SCREEN_WIDTH];
  // ST7735 expects Big Endian RGB565
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
  tft.startWrite();
  tft.setAddrWindow(0, pDraw->y, pDraw->iWidth, 1);
  tft.writePixels(lineBuffer, pDraw->iWidth);
  tft.endWrite();
}

// Draw a PNG file stored in LittleFS to the TFT
bool renderPNGFromLittleFS(const char* filepath) {
  File file = LittleFS.open(filepath, "r");
  if (!file) {
    Serial.printf("[LittleFS] Failed to open %s\n", filepath);
    return false;
  }

  size_t size = file.size();
  uint8_t* buffer = (uint8_t*)malloc(size);
  if (!buffer) {
    Serial.println("[RAM] Allocation failed for PNG buffer");
    file.close();
    return false;
  }

  file.read(buffer, size);
  file.close();

  int rc = png.openRAM(buffer, size, pngDrawCallback);
  if (rc == PNG_SUCCESS) {
    tft.startWrite();
    png.decode(NULL, 0);
    tft.endWrite();
    png.close();
    Serial.println("[Display] Image rendered from flash cache successfully.");
    free(buffer);
    return true;
  } else {
    Serial.printf("[PNGdec] Error decoding file: %d\n", rc);
    free(buffer);
    return false;
  }
}

// Fetch new image from esp32-photoframe-server and cache to LittleFS
bool downloadAndCachePhoto() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  http.begin(SERVER_IMAGE_URL);
  http.setTimeout(10000);

  // Authentication & Format Headers
  String authHeader = String("Bearer ") + DEVICE_TOKEN;
  http.addHeader("Authorization", authHeader);
  http.addHeader("X-Display-Width", String(SCREEN_WIDTH));
  http.addHeader("X-Display-Height", String(SCREEN_HEIGHT));
  http.addHeader("X-Display-Orientation", ORIENTATION);
  http.addHeader("X-Firmware-Version", "1.0.0"); // Enforces standard PNG payload

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[HTTP] Fetch failed with code %d: %s\n", httpCode, http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) {
    http.end();
    return false;
  }

  // Open temporary file in LittleFS for streaming write
  File file = LittleFS.open("/temp.png", "w");
  if (!file) {
    Serial.println("[LittleFS] Failed to create temp file");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buff[512];
  int remaining = len;

  while (http.connected() && (remaining > 0 || len == -1)) {
    size_t size = stream->available();
    if (size) {
      int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
      file.write(buff, c);
      if (remaining > 0) remaining -= c;
    }
    delay(1);
  }
  file.close();
  http.end();

  // Atomically replace cached photo
  if (LittleFS.exists(CACHE_PATH)) {
    LittleFS.remove(CACHE_PATH);
  }
  LittleFS.rename("/temp.png", CACHE_PATH);

  Serial.println("[Sync] Download completed and saved to flash cache.");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Backlight Power ON
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  // Initialize Display (ST7735 Model V1.1)
  tft.initR(INITR_BLACKTAB); // If pixel offset appears, replace with INITR_REDTAB
  tft.setRotation(1);        // Landscape 160x128
  tft.fillScreen(ST77XX_BLACK);

  // Initialize LittleFS Flash Storage
  if (!LittleFS.begin(true)) {
    Serial.println("[LittleFS] Mount Failed");
  }

  // Step 1: Render cached image immediately if available
  if (LittleFS.exists(CACHE_PATH)) {
    Serial.println("[Boot] Loading cached photo from flash...");
    renderPNGFromLittleFS(CACHE_PATH);
  } else {
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(10, 50);
    tft.println("Connecting WiFi...");
  }

  // Step 2: Connect to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(400);
    attempts++;
  }

  // Step 3: Fetch updated image if server is reachable
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected. Checking server...");
    if (downloadAndCachePhoto()) {
      renderPNGFromLittleFS(CACHE_PATH);
    }
  } else {
    Serial.println("[WiFi] Offline. Operating using flash cache.");
  }
}

unsigned long lastSync = 0;

void loop() {
  unsigned long now = millis();
  if (now - lastSync >= REFRESH_INTERVAL_MS) {
    lastSync = now;
    if (WiFi.status() == WL_CONNECTED) {
      if (downloadAndCachePhoto()) {
        renderPNGFromLittleFS(CACHE_PATH);
      }
    } else {
      WiFi.reconnect();
    }
  }
  delay(100);
}
```

---

## 6. Complete Blueprint 2: Standalone Mode (Direct Phone Upload Without Raspberry Pi)

This sketch runs a self-contained Web Server directly on the ESP32-S3. The user opens their phone browser to the ESP32's IP address, selects any image from their phone, and uploads it. **Zero Raspberry Pi or Docker required.**

```cpp
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <PNGdec.h>

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

#define TFT_CS    10
#define TFT_RST    5
#define TFT_DC     6
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_LED    4

#define SCREEN_WIDTH  160
#define SCREEN_HEIGHT 128

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
WebServer server(80);
PNG png;
File uploadFile;

const char* CACHE_PATH = "/uploaded_photo.png";

void pngDrawCallback(PNGDRAW *pDraw) {
  uint16_t lineBuffer[SCREEN_WIDTH];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
  tft.startWrite();
  tft.setAddrWindow(0, pDraw->y, pDraw->iWidth, 1);
  tft.writePixels(lineBuffer, pDraw->iWidth);
  tft.endWrite();
}

bool renderStoredImage() {
  if (!LittleFS.exists(CACHE_PATH)) return false;
  File f = LittleFS.open(CACHE_PATH, "r");
  if (!f) return false;
  size_t s = f.size();
  uint8_t* b = (uint8_t*)malloc(s);
  if (!b) { f.close(); return false; }
  f.read(b, s);
  f.close();

  int rc = png.openRAM(b, s, pngDrawCallback);
  if (rc == PNG_SUCCESS) {
    tft.startWrite();
    png.decode(NULL, 0);
    tft.endWrite();
    png.close();
  }
  free(b);
  return (rc == PNG_SUCCESS);
}

// HTML Web Page for Mobile Upload
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>PhotoFrame Upload</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 20px; background: #f0f2f5; }
    .card { background: white; padding: 25px; border-radius: 12px; max-width: 400px; margin: auto; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }
    h2 { color: #1a73e8; }
    input[type=file] { margin: 20px 0; }
    input[type=submit] { background: #1a73e8; color: white; border: none; padding: 12px 24px; border-radius: 6px; font-size: 16px; cursor: pointer; }
  </style>
</head>
<body>
  <div class="card">
    <h2>1.8" Photo Frame</h2>
    <p>Upload a PNG (160x128 recommended):</p>
    <form method="POST" action="/upload" enctype="multipart/form-data">
      <input type="file" name="update" accept="image/png"><br>
      <input type="submit" value="Display on Frame">
    </form>
  </div>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleFileUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (LittleFS.exists(CACHE_PATH)) LittleFS.remove(CACHE_PATH);
    uploadFile = LittleFS.open(CACHE_PATH, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    server.sendHeader("Location", "/");
    server.send(303);
    renderStoredImage();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  LittleFS.begin(true);
  renderStoredImage();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // Display IP on Screen
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(5, 5);
  tft.print("Web Upload Ready at:");
  tft.setCursor(5, 20);
  tft.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, []() { server.send(200); }, handleFileUpload);
  server.begin();
}

void loop() {
  server.handleClient();
}
```

---

## 7. PlatformIO Build Configuration (`platformio.ini`)

For agents generating a PlatformIO workspace:

```ini
[env:esp32-s3-supermini]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_speed = 921600

build_flags = 
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCORE_DEBUG_LEVEL=0

board_build.filesystem = littlefs
board_build.partitions = default_4MB.csv

lib_deps =
    adafruit/Adafruit ST7735 and ST7789 Library@^1.10.4
    adafruit/Adafruit GFX Library@^1.11.9
    bitbanksoftware/PNGdec@^1.0.1
```
