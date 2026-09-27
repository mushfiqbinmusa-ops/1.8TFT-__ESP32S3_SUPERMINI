# ESP32-S3 SuperMini & ST7735 (1.8" TFT 128×160) PhotoFrame Setup Guide

This guide covers everything you need to connect an **ESP32-S3 SuperMini** microcontroller to a **1.8-inch TFT SPI color display (Model V1.1, ST7735R/ST7735S, 128×160)**, and configure it to work with this **`esp32-photoframe-server`** repository.

---

## Table of Contents
1. [Repository Architecture & Working Principle](#1-repository-architecture--working-principle)
2. [Hardware Overview & Pinout Analysis](#2-hardware-overview--pinout-analysis)
3. [Wiring & Connection Diagram](#3-wiring--connection-diagram)
4. [Server Deployment & Setup](#4-server-deployment--setup)
5. [Software Configuration (Arduino IDE / PlatformIO)](#5-software-configuration-arduino-ide--platformio)
   - [Library Option A: Adafruit_ST7735 + PNGdec (Recommended)](#option-a-adafruit_st7735--pngdec-sketch)
   - [Library Option B: TFT_eSPI Configuration](#option-b-tft_espi-setup)
6. [ST7735 V1.1 Specific Display Quirks (Offset & Colors)](#6-st7735-v11-specific-display-quirks-offset--colors)
7. [Troubleshooting & Pro Tips](#7-troubleshooting--pro-tips)

---

## 1. Repository Architecture & Working Principle

### What this repository is
The [`esp32-photoframe-server`](https://github.com/aitjcize/esp32-photoframe-server) is a server application (written in Go with a Vue 3 web interface) designed to run on a home server (Docker, Raspberry Pi, or Home Assistant Add-on).

### How it handles images for the ESP32
1. **Photo Sources**: It aggregates photos from Google Photos, Immich, Synology Photos, Unsplash, Pexels, AI Generation (OpenAI / Gemini), Telegram/Gallery, or direct URLs.
2. **Auto-Formatting & Overlays**: The server crops, scales, and fits photos to your frame's exact resolution (128×160), with optional weather, date, time, and calendar overlays.
3. **Serving Images**:
   - The ESP32 device makes an HTTP GET request to:  
     `http://<SERVER_IP>:9607/image`
   - Authentication: Header `Authorization: Bearer <DEVICE_TOKEN>` (or query parameter `?token=<DEVICE_TOKEN>`).
   - Resolution Headers: The client sends `X-Display-Width`, `X-Display-Height`, and `X-Display-Orientation`.
   - Output Format: For standard firmware (non-e-paper), the server automatically renders and serves a clean, standard **PNG image (`image/png`)**.
4. **On-Device Decoding**: The ESP32-S3 receives the PNG over Wi-Fi, decodes it into 16-bit RGB565 format line-by-line, and pushes it directly into the ST7735 display buffer.

---

## 2. Hardware Overview & Pinout Analysis

### 2.1. ESP32-S3 SuperMini
The ESP32-S3 SuperMini is an ultra-compact development board featuring the **ESP32-S3FH4R2** SoC (Dual-Core Xtensa LX7 up to 240 MHz, 4MB Flash, 2MB embedded QSPI PSRAM).
- **Logic Voltage**: **3.3V strictly**. (Do not feed 5V into GPIO pins).
- **USB-C**: Connects directly to the ESP32-S3 native USB (CDC / JTAG).
- **Onboard WS2812 RGB LED**: Connected to **GPIO 48**.
- **Hardware SPI (FSPI)**:
  - Default Clock (`SCK`): **GPIO 12**
  - Default MOSI (`SDA`/Data Out): **GPIO 11**
  - Default MISO (`Data In`): **GPIO 13**
  - Default CS (`Chip Select`): **GPIO 10**

> [!NOTE]
> The ESP32-S3 has a full GPIO Matrix, meaning SPI signals can be routed to almost any pin, but using native hardware SPI pins (GPIO 11 & 12) ensures maximum SPI throughput (up to 40-80 MHz).

### 2.2. 1.8-inch TFT SPI Display Module (Model V1.1)
- **Driver IC**: Sitronix **ST7735R** / **ST7735S**
- **Resolution**: 128 × 160 pixels
- **Color Depth**: 16-bit RGB565 (65,536 colors)
- **Logic Level**: 3.3V (directly compatible with ESP32-S3)
- **Typical 8-Pin Header Pins**:
  - `VCC`: Module power (3.3V or 5V; connect to 3.3V or 5V on board)
  - `GND`: Ground (0V)
  - `CS`: SPI Chip Select (Active LOW)
  - `RESET` / `RES`: Hardware Reset (Active LOW)
  - `A0` / `DC`: Data / Command control line
  - `SDA` / `MOSI`: SPI Serial Data input
  - `SCK` / `SCL`: SPI Serial Clock
  - `LED` / `BLK`: Display Backlight anode

---

## 3. Wiring & Connection Diagram

### 3.1. Pin Mapping Table

| ST7735 1.8" TFT (V1.1) Pin | ESP32-S3 SuperMini Pin | Function Description | Notes |
|:---|:---|:---|:---|
| **VCC** | **3.3V** (or **5V**) | Power Supply | 3.3V pin is ideal; 5V is fine if module has XC6206 regulator |
| **GND** | **GND** | Ground | Common system ground |
| **CS** | **GPIO 10** | SPI Chip Select | Active LOW |
| **RESET** (RES) | **GPIO 5** | Hardware Reset | Active LOW |
| **A0** (DC) | **GPIO 6** | Data / Command | Low = Command, High = Data |
| **SDA** (MOSI) | **GPIO 11** | SPI MOSI Data | Hardware SPI MOSI |
| **SCK** (SCL) | **GPIO 12** | SPI Clock | Hardware SPI Clock |
| **LED** (BLK) | **3.3V** or **GPIO 4** | Backlight Power | Connect to 3.3V for always-on, or GPIO 4 for PWM dimming |

*(If your module has an integrated SD card slot on the back, those pins are separate and optional).*

---

### 3.2. Circuit Wiring Diagram

```
+------------------------------------+          +-------------------------------+
|       ESP32-S3 SuperMini           |          |  1.8" TFT SPI Module (V1.1)   |
|                                    |          |  (ST7735R / ST7735S 128x160)  |
|                         [ 3.3V ]---+--------->| VCC                           |
|                         [  GND ]---+--------->| GND                           |
|                         [ GPIO10]--+--------->| CS                            |
|                         [ GPIO5 ]--+--------->| RESET / RES                   |
|                         [ GPIO6 ]--+--------->| A0 / DC                       |
|                         [ GPIO11]--+--------->| SDA / MOSI                    |
|                         [ GPIO12]--+--------->| SCK / SCL                     |
|                         [ GPIO4 ]--+--------->| LED / BLK (or to 3.3V)        |
+------------------------------------+          +-------------------------------+
```

---

## 4. Server Deployment & Setup

Before flashing the ESP32, set up and configure your photo server so it has images ready to serve.

### Step 4.1: Run the Server via Docker

Create a `docker-compose.yml` or use the one in this repository:

```yaml
version: '3.8'
services:
  photoframe-server:
    image: aitjcize/esp32-photoframe-server:latest
    container_name: photoframe-server
    restart: unless-stopped
    ports:
      - "9607:9607"
    volumes:
      - ./data:/data
    environment:
      - DB_PATH=/data/photoframe.db
```

Launch the container:
```bash
docker compose up -d
```
The web dashboard is now accessible at `http://<SERVER_IP>:9607`.

*(Alternatively, install it as a Home Assistant Add-on by adding repository `https://github.com/aitjcize/esp32-photoframe-server` in the Add-on Store).*

---

### Step 4.2: Configure Web Dashboard & Photo Source
1. Open `http://<YOUR_SERVER_IP>:9607` in your browser.
2. Complete initial admin account creation.
3. Go to **Settings** → **Data Sources**:
   - For an immediate test with zero setup, configure **Unsplash** or **Pexels** by adding a topic like `nature` or `minimalist`.
   - Or configure **Gallery** and upload photos directly.
   - Or configure **Immich**, **Google Photos**, or **Synology Photos**.
4. Click **Sync Now** to pull metadata and photos.

---

### Step 4.3: Add Device & Generate API Token
1. Go to **Settings** → **Devices** → **Add Device**:
   - **Name**: `SuperMini 1.8 TFT`
   - **Host**: Enter your ESP32's expected IP or hostname (e.g. `esp32-photoframe.local` or placeholder IP).
   - **Orientation**: `landscape` (or `portrait`).
   - **Width**: `160` (or `128` if portrait).
   - **Height**: `128` (or `160` if portrait).
   - **Source**: Select your configured photo source (e.g. `unsplash` or `gallery`).
   - Under **Overlays**, you can turn on Clock/Date if desired.
2. Go to **Settings** → **Security**:
   - Under **Device Tokens**, create a new token:
     - Name: `esp32-supermini`
     - Device: Select `SuperMini 1.8 TFT`
   - Click **Generate Token**.
   - **Copy the generated Bearer Token** (e.g. `eyJhbGciOi...`). You will paste this into the ESP32 sketch.

---

## 5. Software Configuration (Arduino IDE / PlatformIO)

### Option A: Adafruit_ST7735 + PNGdec Sketch

This method is recommended for reliability and simplicity.

#### Required Libraries (Install via Arduino Library Manager):
1. **`Adafruit ST7735 and ST7789 Library`** (by Adafruit)
2. **`Adafruit GFX Library`** (by Adafruit)
3. **`PNGdec`** (by Larry Bank) — Ultra-fast, lightweight PNG decoder for microcontrollers
4. **`WiFi`** & **`HTTPClient`** (Built-in to ESP32 core)

#### Arduino IDE Board Settings (under `Tools` menu):
- **Board**: `ESP32S3 Dev Module` (or `Waveshare ESP32-S3-Zero` / `LOLIN S3 MINI`)
- **USB CDC On Boot**: **Enabled** *(Crucial for Serial.print output over USB-C)*
- **Flash Size**: `4MB (32Mb)`
- **PSRAM**: `QSPI PSRAM` (or `Disabled` if your board variant has no PSRAM)
- **Upload Mode**: `UART0 / Hardware CDC`
- **Upload Speed**: `921600`

---

#### Arduino Code: `esp32_s3_photoframe.ino`

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <PNGdec.h>

// ==========================================
//           NETWORK & SERVER CONFIG
// ==========================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Server URL (Port 9607)
const char* SERVER_IMAGE_URL = "http://192.168.1.100:9607/image";

// Paste your Device Token generated in Settings -> Security
const char* DEVICE_TOKEN     = "YOUR_DEVICE_TOKEN_HERE";

// Refresh Interval (in milliseconds) - e.g. 5 minutes
const unsigned long REFRESH_INTERVAL_MS = 5 * 60 * 1000;

// ==========================================
//             PIN DEFINITIONS
// ==========================================
#define TFT_CS    10  // Chip Select
#define TFT_RST    5  // Reset
#define TFT_DC     6  // Data/Command (A0)
#define TFT_MOSI  11  // SPI MOSI (SDA)
#define TFT_SCLK  12  // SPI Clock (SCK)
#define TFT_LED    4  // Backlight control (Optional, or connect to 3.3V)

// Screen Dimensions (Landscape: 160x128, Portrait: 128x160)
#define SCREEN_WIDTH   160
#define SCREEN_HEIGHT  128
#define ORIENTATION    "landscape"

// Initialize Display
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
PNG png;

// Line rendering callback for PNGdec
void pngDrawCallback(PNGDRAW *pDraw) {
  uint16_t lineBuffer[SCREEN_WIDTH];
  // Convert line from PNG to RGB565 Big Endian for ST7735
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
  tft.startWrite();
  tft.setAddrWindow(0, pDraw->y, pDraw->iWidth, 1);
  tft.writePixels(lineBuffer, pDraw->iWidth);
  tft.endWrite();
}

void connectWiFi() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 10);
  tft.println("Connecting to WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    tft.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    tft.println("\nConnected!");
    tft.print("IP: ");
    tft.println(WiFi.localIP());
    delay(1000);
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.println("\nWiFi Failed!");
    delay(3000);
  }
}

bool fetchAndDisplayPhoto() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return false;
  }

  HTTPClient http;
  http.begin(SERVER_IMAGE_URL);
  
  // Set Authorization Bearer Token
  String authHeader = String("Bearer ") + DEVICE_TOKEN;
  http.addHeader("Authorization", authHeader);

  // Set resolution and orientation headers so server formats the image exactly
  http.addHeader("X-Display-Width", String(SCREEN_WIDTH));
  http.addHeader("X-Display-Height", String(SCREEN_HEIGHT));
  http.addHeader("X-Display-Orientation", ORIENTATION);
  http.addHeader("X-Firmware-Version", "1.0.0"); // Triggers PNG output from server

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[HTTP] GET failed, error: %s (code %d)\n", http.errorToString(httpCode).c_str(), httpCode);
    tft.setCursor(5, 50);
    tft.setTextColor(ST77XX_RED);
    tft.printf("HTTP Error: %d\n", httpCode);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) {
    Serial.println("[HTTP] Invalid payload size");
    http.end();
    return false;
  }

  // Allocate buffer for image (PNGs for 160x128 are typically 5KB to 35KB)
  uint8_t* imgBuffer = (uint8_t*)malloc(len);
  if (!imgBuffer) {
    Serial.println("[Memory] Not enough RAM to allocate image buffer");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  int bytesRead = 0;
  while (http.connected() && (bytesRead < len)) {
    size_t availableBytes = stream->available();
    if (availableBytes) {
      int c = stream->readBytes(imgBuffer + bytesRead, availableBytes);
      bytesRead += c;
    }
    delay(1);
  }
  http.end();

  Serial.printf("[Image] Downloaded %d bytes. Decoding PNG...\n", bytesRead);

  // Open and decode PNG directly to TFT
  int rc = png.openRAM(imgBuffer, bytesRead, pngDrawCallback);
  if (rc == PNG_SUCCESS) {
    tft.startWrite();
    png.decode(NULL, 0);
    tft.endWrite();
    png.close();
    Serial.println("[Image] Display updated successfully!");
  } else {
    Serial.printf("[PNG] Decode error code: %d\n", rc);
    tft.setCursor(5, 50);
    tft.setTextColor(ST77XX_RED);
    tft.printf("PNG Decode Error: %d\n", rc);
  }

  free(imgBuffer);
  return (rc == PNG_SUCCESS);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Turn on Backlight if wired to GPIO
  #if defined(TFT_LED)
    pinMode(TFT_LED, OUTPUT);
    digitalWrite(TFT_LED, HIGH);
  #endif

  // Initialize ST7735
  // Model V1.1 typically uses INITR_BLACKTAB or INITR_REDTAB
  tft.initR(INITR_BLACKTAB); 
  
  // Set orientation: 1 or 3 = Landscape (160x128), 0 or 2 = Portrait (128x160)
  tft.setRotation(1); 
  tft.fillScreen(ST77XX_BLACK);

  // Connect to Wi-Fi and fetch first picture
  connectWiFi();
  fetchAndDisplayPhoto();
}

unsigned long lastUpdate = 0;

void loop() {
  unsigned long now = millis();
  if (now - lastUpdate >= REFRESH_INTERVAL_MS) {
    lastUpdate = now;
    fetchAndDisplayPhoto();
  }
  delay(100);
}
```

---

### Option B: TFT_eSPI Setup

If you prefer `TFT_eSPI` for hardware DMA acceleration:

1. Install **`TFT_eSPI`** library in Arduino IDE.
2. Edit `Arduino/libraries/TFT_eSPI/User_Setup.h` (or create a custom configuration):

```c
// In User_Setup.h:
#define ST7735_DRIVER      // 1.8" TFT Driver
#define TFT_WIDTH  128
#define TFT_HEIGHT 160

#define ST7735_BLACKTAB    // Change to ST7735_REDTAB if colors/offsets shift

// ESP32-S3 Hardware Pins
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    6
#define TFT_RST   5
#define TFT_BL    4

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define SPI_FREQUENCY  27000000 // 27 MHz SPI
```

---

## 6. ST7735 V1.1 Specific Display Quirks (Offset & Colors)

The ST7735 1.8" V1.1 module was produced in multiple hardware revisions differing in glass subpixel arrangement. If your display shows glitches, apply the following fixes:

### 1. Pixel Offset (Border artifact of 1-3 random colored pixel lines)
- **Symptom**: Image is slightly shifted off the top or left edge with noisy static pixels on one side.
- **Fix in `Adafruit_ST7735`**:
  In `setup()`, change:
  ```cpp
  tft.initR(INITR_BLACKTAB);
  ```
  to:
  ```cpp
  tft.initR(INITR_REDTAB);   // Try Red Tab
  // OR:
  tft.initR(INITR_GREENTAB); // Try Green Tab
  ```
- **Fix in `TFT_eSPI`**: Change `#define ST7735_BLACKTAB` to `#define ST7735_REDTAB`.

### 2. Inverted / Negative Colors
- **Symptom**: Whites appear black, faces look blue or inverted.
- **Fix**: In `setup()`, toggle display inversion:
  ```cpp
  tft.invertDisplay(true); // Call after tft.initR()
  ```

### 3. Red and Blue Channels Swapped (BGR vs RGB)
- If reds look blue and blues look red, adjust `PNGdec` line conversion:
  Change `PNG_RGB565_BIG_ENDIAN` to `PNG_RGB565_LITTLE_ENDIAN` in `pngDrawCallback()`.

---

## 7. Troubleshooting & Pro Tips

| Issue | Cause | Solution |
|:---|:---|:---|
| **White Screen Only** | Backlight on, but no SPI communication or display uninitialized. | 1. Verify `SCK -> GPIO 12`, `SDA -> GPIO 11`, `CS -> GPIO 10`.<br>2. Check `GND` connection.<br>3. Verify `A0 / DC` is on `GPIO 6`. |
| **No Serial Monitor Output** | ESP32-S3 native USB CDC disabled by default. | In Arduino IDE, go to **Tools** → **USB CDC On Boot** → **Enabled**. |
| **Flashing / Upload Fails** | ESP32-S3 ROM bootloader not triggered. | Hold **BOOT** button, plug in USB cable (or tap **RESET** button while holding **BOOT**), then release **BOOT**. Upload again. |
| **HTTP 400 Bad Request** | No image source assigned to device. | In the server dashboard, go to **Settings** → **Devices**, select your device, and select an **Image Source** (e.g., Unsplash, Gallery). |
| **HTTP 401 Unauthorized** | Token mismatch or missing Bearer header. | Verify the `DEVICE_TOKEN` string in the sketch matches the token in **Settings** → **Security**. |
| **Weak Wi-Fi Connection** | Known ceramic antenna issue on SuperMini. | Some SuperMini boards have misaligned ceramic antenna traces. Stay close to router or solder a 31mm thin wire as an antenna mod. |
