// =============================================================================
//  STAGE 1 SMOKE TEST  --  ESP32-S3 SuperMini + 1.8" ST7735 TFT (V1.1)
// =============================================================================
//  Purpose: prove the hardware before a single line of photo-frame logic exists.
//
//  It checks, in order:
//    1. Backlight GPIO (does LED/BLK on GPIO 4 light the panel?)
//    2. SPI + ST7735 init  (does the controller respond?)
//    3. Panel geometry     (is it 160x128 landscape?)
//    4. RGB565 colour order (are red and blue swapped?)
//    5. Readback            (the diagnostics below require a Display On module)
//
//  It does NOT touch Wi-Fi, the server, or LittleFS.
//
//  --- WHAT TO REPORT BACK -----------------------------------------------------
//  Open Tools -> Serial Monitor at 115200 baud and paste the whole output, plus
//  answer:  (a) did the backlight come on?  (b) is the image shifted / are there
//  stray coloured lines?  (c) are colours negative?  (d) does RED look red?
//  (e) does the top-left corner show RED and the top-right BLUE?
// =============================================================================

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// -----------------------------------------------------------------------------
//  PINS  --  must match the wiring table in ../README.md
// -----------------------------------------------------------------------------
#define TFT_CS    10   // -> TFT CS      (active low)
#define TFT_RST    5   // -> TFT RESET  (active low)
#define TFT_DC     6   // -> TFT A0/DC   (low = command, high = data)
#define TFT_MOSI  11   // -> TFT SDA     (FSPI data out)
#define TFT_SCLK  12   // -> TFT SCK     (FSPI clock)
#define TFT_LED    4   // -> TFT LED/BLK (backlight; HIGH = on)

#define BACKLIGHT_ON_WIFI  false  // set true only if you tied LED to 3.3V
                                  // and are driving it from elsewhere

// -----------------------------------------------------------------------------
//  PANEL
// -----------------------------------------------------------------------------
//  The 1.8" V1.1 module is physically 128x160. In landscape it reports 160x128.
//  Start with INITR_BLACKTAB; if the image is offset by a pixel or two with
//  stray coloured lines, switch to INITR_REDTAB (see ../README.md, Troubleshooting).
#define TAB_INIT   INITR_BLACKTAB
#define ROTATION   1          // 1 or 3 = landscape 160x128
                               // 0 or 2 = portrait  128x160

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

static const uint32_t SERIAL_BAUD = 115200;

static void banner(const char* label) {
  Serial.println();
  Serial.println("--------------------------------------------------------------");
  Serial.print("  ");
  Serial.println(label);
  Serial.println("--------------------------------------------------------------");
}

static void reportGeometry() {
  Serial.print("[1] Geometry      : ");
  Serial.print(tft.width());
  Serial.print(" x ");
  Serial.print(tft.height());
  Serial.print("   (rotation ");
  Serial.print(ROTATION);
  Serial.println(")");
  Serial.print("    expected        : 160 x 128  ->  ");
  Serial.println((tft.width() == 160 && tft.height() == 128) ? "OK" : "MISMATCH (change ROTATION)");
}

static void drawTestPattern() {
  // 1. Solid white -- proves the backlight is actually lighting the glass
  tft.fillScreen(ST77XX_WHITE);
  delay(1200);

  // 2. Three vertical bars. On a healthy RGB565 panel the left bar is RED and
  //    the right bar is BLUE. If they are swapped, see the endianness note in
  //    ../README.md -- that is fixed in the real firmware, not here.
  tft.fillRect(0, 0, tft.width() / 3, tft.height(), ST77XX_RED);
  delay(1200);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(tft.width() / 3, 0, tft.width() / 3, tft.height(), ST77XX_GREEN);
  delay(1200);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(2 * (tft.width() / 3), 0, tft.width() / 3, tft.height(), ST77XX_BLUE);
  delay(1200);

  // 3. Corner markers: the top-left pixel is forced red and the top-right blue.
  //    If they are not, the panel's address window is off -- that is the
  //    "1-2 stray coloured lines on one edge" symptom.
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, 3, 3, ST77XX_RED);
  tft.fillRect(tft.width() - 3, 0, 3, 3, ST77XX_BLUE);
  tft.fillRect(0, tft.height() - 3, 3, 3, ST77XX_GREEN);
  tft.fillRect(tft.width() - 3, tft.height() - 3, 3, 3, ST77XX_YELLOW);
  delay(1500);

  // 4. A one-pixel-wide diagonal and a fine grid. Reveals SPI bit-order errors
  //    (garbled noise) and a mis-sized address window (a torn or wrapped image).
  tft.fillScreen(ST77XX_BLACK);
  for (int16_t y = 0; y < tft.height(); y++) {
    tft.drawPixel(y, y, ST77XX_WHITE);          // diagonal
    tft.drawPixel(tft.width() - 1 - y, y, ST77XX_CYAN);
  }
  for (int16_t x = 0; x < tft.width(); x += 8) {
    tft.drawFastVLine(x, 0, tft.height(), ST77XX_BLUE);
  }
  for (int16_t y = 0; y < tft.height(); y += 8) {
    tft.drawFastHLine(0, y, tft.width(), ST77XX_BLUE);
  }
  delay(1500);

  // 5. Colour-order reference card. A correct panel shows, left to right:
  //    black, red, green, yellow, blue, magenta, cyan, white.
  const uint16_t ramp[8] = {
    ST77XX_BLACK,  ST77XX_RED,    ST77XX_GREEN, ST77XX_YELLOW,
    ST77XX_BLUE,   ST77XX_MAGENTA, ST77XX_CYAN,  ST77XX_WHITE
  };
  const int16_t bandH = tft.height() / 2;
  tft.fillScreen(ST77XX_BLACK);
  for (uint8_t i = 0; i < 8; i++) {
    tft.fillRect(i * (tft.width() / 8), 0, tft.width() / 8 + 1, bandH, ramp[i]);
  }
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(0, bandH + 4);
  tft.print("K R G Y B M C W");
  tft.setCursor(0, bandH + 16);
  tft.print("st7735 smoke test");
  delay(4000);
}

// -----------------------------------------------------------------------------
//  setup
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1200);   // let the USB CDC enumeration settle before we print

  banner("ST7735 SMOKE TEST -- no Wi-Fi, no server, no flash writes");

  // ---- [0] Backlight ------------------------------------------------------
  //  If you wired LED/BLK straight to 3.3V instead of GPIO 4, the panel lights
  //  up on its own and this section is a no-op.
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);
  Serial.print("[0] Backlight     : GPIO ");
  Serial.print(TFT_LED);
  Serial.println(" driven HIGH");
#if BACKLIGHT_ON_WIFI
  Serial.println("    NOTE: BACKLIGHT_ON_WIFI is true -- pin is held LOW by the");
  Serial.println("          sketch. Expected behaviour is a DARK panel.");
#endif

  // ---- [1] Panel init -----------------------------------------------------
  tft.initR(TAB_INIT);
  tft.setRotation(ROTATION);
  tft.fillScreen(ST77XX_BLACK);
  reportGeometry();
  Serial.println();
  Serial.println("[2] SPI + init    : no exception thrown (see the pattern below)");
  Serial.println();
  Serial.println("Running the test pattern. It loops forever.");
  Serial.println("Copy this output into the chat so the layout can be checked.");
  Serial.println();

  drawTestPattern();
}

// -----------------------------------------------------------------------------
//  loop -- nothing to do; the smoke test is fully static
// -----------------------------------------------------------------------------
void loop() {
  drawTestPattern();
  delay(1000);
}
