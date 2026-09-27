// =============================================================================
//  GATE 1 SMOKE TEST  --  ESP32-S3 SuperMini + 1.8" ST7735 TFT (V1.1)
// =============================================================================
//  Purpose: prove the hardware before a single line of photo-frame logic exists.
//
//  Touches no Wi-Fi, no server and no flash storage. Pins, rotation, panel tab
//  and backlight all come from esp32_s3_photoframe/config.h so there is one
//  source of truth, and a fix that works here works in the real firmware.
//
//  IMPORTANT -- what this test can and cannot prove:
//  SPI has no acknowledgement, and Adafruit_ST7735::initR() returns void. The
//  library will blit a whole test pattern into a completely dead bus without
//  noticing. So NOTHING on the serial log can tell you the display is alive.
//  The only evidence that matters is what you see with your eyes.
//
//  Every phase is announced on serial at the moment it is displayed, and held
//  long enough to recognise, so you can match what is on the glass to what is
//  on the terminal.
//
//  --- WHAT TO REPORT BACK -----------------------------------------------------
//  Paste the serial output, then answer, for the pattern you can see:
//    (a) is the backlight now properly bright?
//    (b) is the image shifted, or are there stray coloured lines on one edge?
//    (c) do colours look negative (does white render black)?
//    (d) in the reference card, is the leftmost band BLACK and the rightmost
//        WHITE?
//    (e) is the left band RED and the fifth band BLUE?
// =============================================================================

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include "config.h"

static const uint32_t SERIAL_BAUD = 115200;

//  Note the argument order: (cs, dc, mosi, sck, rst). The pin names below are in
//  a different order, which is exactly the kind of thing that gets misread and
//  mis-wired, so it is spelled out here rather than left implicit.
static Adafruit_ST7735 tft = Adafruit_ST7735(
    PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_MOSI, PIN_TFT_SCLK, PIN_TFT_RST);

// -----------------------------------------------------------------------------
//  Backlight
//
//  With BACKLIGHT_ON_GPIO4 false the panel's BLK pin sits on the 3.3V rail and is
//  lit by the regulator. The firmware must not touch GPIO 4 at all in that case:
//  driving it LOW while the pin is still wired to the panel would short the rail
//  through the module.
// -----------------------------------------------------------------------------
static void setupBacklight() {
  Serial.println("[0] Backlight     :");
#if BACKLIGHT_ON_GPIO4
  pinMode(PIN_TFT_LED, OUTPUT);
  digitalWrite(PIN_TFT_LED, HIGH);
  Serial.print("    externally gated OFF, driving GPIO ");
  Serial.print(PIN_TFT_LED);
  Serial.println(" HIGH from the firmware");
  Serial.println("    NOTE: a GPIO sources only ~8-12 mA and this backlight wants");
  Serial.println("          ~20-40 mA, so expect it to be very dim. Moving BLK to");
  Serial.println("          the 3.3V rail and setting BACKLIGHT_ON_GPIO4 false in");
  Serial.println("          config.h fixes it properly.");
#else
  Serial.println("    external -- BLK/LED is wired to the 3.3V rail, so the panel is");
  Serial.print("    lit by the regulator. GPIO ");
  Serial.print(PIN_TFT_LED);
  Serial.println(" is left untouched (must stay disconnected from the panel).");
#endif
}

// -----------------------------------------------------------------------------
//  Diagnostics
// -----------------------------------------------------------------------------

//  Announce a phase on serial at the moment it starts being displayed.
static void phase(const char* label) {
  Serial.print("    [showing] ");
  Serial.println(label);
  Serial.flush();
}

static void reportGeometry() {
  Serial.print("    size reported by the library: ");
  Serial.print(tft.width());
  Serial.print(" x ");
  Serial.println(tft.height());
  Serial.println("    (this is a compile-time constant from setRotation(), NOT a");
  Serial.println("     measurement. It does not prove the panel responded.)");
  Serial.print("    rotation ");
  Serial.print(TFT_ROTATION);
  Serial.print(", tab ");
  Serial.println(TFT_TAB == INITR_BLACKTAB   ? "BLACKTAB"
                : TFT_TAB == INITR_REDTAB   ? "REDTAB"
                : TFT_TAB == INITR_GREENTAB ? "GREENTAB"
                                            : "other");
}

// -----------------------------------------------------------------------------
//  Test pattern
// -----------------------------------------------------------------------------
static void drawTestPattern() {
  // --- 1. Solid white -------------------------------------------------------
  //  A full-brightness flood is the single most useful frame: if this is not
  //  white, nothing else in the pattern means anything yet.
  phase("SOLID WHITE  (5s) -- is it bright white?");
  tft.fillScreen(ST77XX_WHITE);
  delay(5000);

  // --- 2. Three vertical colour bars ---------------------------------------
  //  Left bar red, middle green, right blue. Swapped red/blue means an RGB565
  //  byte-order problem, fixed with PNG_BIG_ENDIAN in config.h.
  phase("RED | GREEN | BLUE  (4s) -- is the LEFT bar red and the RIGHT blue?");
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, tft.width() / 3, tft.height(), ST77XX_RED);
  tft.fillRect(tft.width() / 3, 0, tft.width() / 3, tft.height(), ST77XX_GREEN);
  tft.fillRect(2 * (tft.width() / 3), 0, tft.width() / 3, tft.height(), ST77XX_BLUE);
  delay(4000);

  // --- 3. Corner markers ----------------------------------------------------
  //  Forces a known colour into each corner. The wrong colour in a corner means
  //  the panel's address window is offset -- the "stray coloured lines along one
  //  edge" symptom -- fixed with TFT_TAB in config.h.
  phase("CORNER MARKERS  (4s) -- red TL, blue TR, green BL, yellow BR?");
  tft.fillScreen(ST77XX_BLACK);
  const int m = 6;
  tft.fillRect(0, 0, m, m, ST77XX_RED);                          // top-left
  tft.fillRect(tft.width() - m, 0, m, m, ST77XX_BLUE);           // top-right
  tft.fillRect(0, tft.height() - m, m, m, ST77XX_GREEN);          // bottom-left
  tft.fillRect(tft.width() - m, tft.height() - m, m, m, ST77XX_YELLOW);
  delay(4000);

  // --- 4. Diagonal and grid -------------------------------------------------
  //  A near-infinite sweep of the whole address window. Torn, wrapped or
  //  striped output here points at the SPI link rather than the panel variant.
  phase("DIAGONAL + GRID  (4s) -- solid, not torn or striped?");
  tft.fillScreen(ST77XX_BLACK);
  for (int16_t y = 0; y < tft.height(); y++) {
    tft.drawPixel(y, y, ST77XX_WHITE);
    tft.drawPixel(tft.width() - 1 - y, y, ST77XX_CYAN);
  }
  for (int16_t x = 0; x < tft.width(); x += 8) {
    tft.drawFastVLine(x, 0, tft.height(), ST77XX_BLUE);
  }
  for (int16_t y = 0; y < tft.height(); y += 8) {
    tft.drawFastHLine(0, y, tft.width(), ST77XX_BLUE);
  }
  delay(4000);

  // --- 5. Colour-order reference card ---------------------------------------
  //  Left to right: black, red, green, yellow, blue, magenta, cyan, white.
  //  A correct panel shows black on the left and white on the right. Inverted
  //  ends (white left, black right) mean TFT_INVERT in config.h needs flipping.
  phase("COLOUR RAMP CARD  (6s) -- leftmost band BLACK, rightmost WHITE?");
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
  tft.setCursor(0, bandH + 6);
  tft.print("K R G Y B M C W");
  tft.setCursor(0, bandH + 18);
  tft.print("st7735 gate 1");
  delay(6000);
}

// -----------------------------------------------------------------------------
//  setup
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1500);   // let USB CDC enumerate before the first print, or it is lost

  Serial.println();
  Serial.println("--------------------------------------------------------------");
  Serial.println("  ST7735 SMOKE TEST -- no Wi-Fi, no server, no flash writes");
  Serial.println("--------------------------------------------------------------");

  setupBacklight();

  // Panel init. No return value to check, by design of the library.
  tft.initR(TFT_TAB);
  tft.setRotation(TFT_ROTATION);
  if (TFT_INVERT) tft.invertDisplay(true);

  Serial.println();
  Serial.println("[1] Panel init sent.");
  reportGeometry();

  Serial.println();
  Serial.println("[2] NOTE: this test cannot confirm the panel is alive.");
  Serial.println("    SPI has no acknowledgement and initR() returns void, so a");
  Serial.println("    completely dead bus still looks identical here. Trust your eyes");
  Serial.println("    on the display, not this log.");

  Serial.println();
  Serial.println("Phases repeat forever. Compare the panel to the [showing] lines.");
  Serial.println();

  drawTestPattern();
}

// -----------------------------------------------------------------------------
//  loop -- the pattern is fully static, just cycle it
// -----------------------------------------------------------------------------
void loop() {
  drawTestPattern();
  delay(500);
}
