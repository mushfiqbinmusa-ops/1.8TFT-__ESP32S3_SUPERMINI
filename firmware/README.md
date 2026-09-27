# ESP32-S3 SuperMini + ST7735 PhotoFrame Firmware

Firmware for driving a 1.8" ST7735 TFT colour display as a photo frame, fed by
the `esp32-photoframe-server` backend in this repository.

The server does the hard work — it picks a photo from your library, crops and
scales it to the panel, renders any clock/date/weather overlay, and serves the
result as a PNG. The firmware pulls that PNG over Wi-Fi, caches it in flash, and
pushes it to the display. If the server is unreachable, the frame keeps showing
the cached photo, so the Pi only has to be on when you are changing pictures.

> This is **Plan 1** of the two sketched in `../../.agent/`. It requires a
> running server. (`../../.agent/AI_AGENT_CODING_INSTRUCTIONS.md:314` describes
> Plan 2, a standalone web-upload mode with no server at all.)

---

## 1. Hardware

| Part | Spec |
|---|---|
| MCU | ESP32-S3 SuperMini (ESP32-S3FH4R2, 4 MB flash, 2 MB QSPI PSRAM) |
| Display | 1.8" TFT SPI module, Model V1.1, ST7735R/ST7735S, 128×160, RGB565 |

**Logic voltage is 3.3 V strictly.** Do not feed 5 V into a GPIO.

### Pin map

| TFT pin | ESP32-S3 | Function | Note |
|---|---|---|---|
| `VCC` | `3.3V` | Power | 5 V also fine if the module has an XC6206 regulator |
| `GND` | `GND` | Ground | must be common with the board |
| `CS` | `GPIO 10` | Chip select | active low |
| `RESET` / `RES` | `GPIO 5` | Hardware reset | active low |
| `A0` / `DC` | `GPIO 6` | Data / command | low = command, high = data |
| `SDA` / `MOSI` | `GPIO 11` | SPI data out | native FSPI |
| `SCK` / `SCL` | `GPIO 12` | SPI clock | native FSPI |
| `LED` / `BLK` | `3.3V` | Backlight | **on the 3.3 V rail, not a GPIO** — see below |
| — | `GPIO 4` | *unused* | left unconnected; do not drive it |

```
+-------------------+            +------------------------+
|  ESP32-S3 SuperMini |            |  1.8" TFT SPI (V1.1)    |
|                     |            |  ST7735R  128x160       |
|            [ 3.3V ]-+------------>| VCC                     |
|            [  GND ]-+------------>| GND                     |
|            [GPIO10]-+------------>| CS                      |
|            [GPIO 5]-+------------>| RESET / RES             |
|            [GPIO 6]-+------------>| A0 / DC                 |
|            [GPIO11]-+------------>| SDA / MOSI              |
|            [GPIO12]-+------------>| SCK / SCL               |
|            [ 3.3V ]-+------------>| LED / BLK               |
+-------------------+            +------------------------+
```

**The backlight is on 3.3 V, and GPIO 4 must stay disconnected.** This is not
a stylistic choice. An ESP32-S3 pin sources roughly 8-12 mA reliably (20 mA
absolute maximum); the 1.8" panel's backlight wants 20-40 mA. Driving `BLK` from
GPIO 4 "works" in the sense that the panel lights, but so faintly you have to
look for it, and no firmware change can fix that — it is a current limit, not a
logic fault.

Worse, with `BLK` on the 3.3 V rail, anything that drove GPIO 4 LOW while it was
still wired would short the rail to ground through the module. Hence
`BACKLIGHT_ON_GPIO4 false` in `config.h`: the firmware never touches the pin.

If you *do* move `BLK` to GPIO 4 (for PWM dimming), set
`BACKLIGHT_ON_GPIO4 true` and confirm the module has its own series resistor on
the backlight first, so the LED survives being driven from a pin at all.

**Do not use** GPIO 0 (BOOT + strapping), 3 (strapping), 19/20 (native USB),
43/44 (UART0), or 48 (onboard WS2812).

If your module's silkscreen labels differ, the mapping to follow is the
*function*, not the label: chip select, reset, data/command, MOSI, clock,
backlight.

---

## 2. Build setup

**Build with PlatformIO, not the Arduino IDE.** The Arduino IDE's builder on
this machine dies before it reaches the compiler:

```
panic: runtime error: index out of range [7] with length 7
  arduino.cc/builder.(*includeCache).Next(...)
  arduino.cc/builder/container_find_includes.go:209
```

This is a bug in the installed builder, not in the sketch. It reproduces with a
one-line sketch, from both the repo and the sketchbook, so nothing about the
wiring, the pin choices or the libraries can be diagnosed through it. PlatformIO
has its own builder and compiles the identical code without complaint.

One-time setup:

```bash
python3 -m venv ~/pio-venv
~/pio-venv/bin/pip install platformio
```

Then everything is driven from `firmware/platformio.ini`:

| Env | Source | Purpose |
|---|---|---|
| `smoke` | `src/smoke.cpp` | Gate 1 wiring test |
| `supermini` | `src/main.cpp` | the firmware |

```bash
cd firmware
~/pio-venv/bin/pio run -e smoke -t upload      # build + flash Gate 1
~/pio-venv/bin/pio run -e supermini -t upload  # build + flash the firmware
~/pio-venv/bin/pio device monitor              # serial monitor @ 115200
```

`boards/esp32-s3-supermini.json` pins the hardware the stock board list gets
wrong: 4 MB flash, `qio_qspi` memory type for the 2 MB QSPI PSRAM, and
`default.csv` for the 4 MB partition table.

Libraries are resolved automatically from `lib_deps` — `Adafruit ST7735 and
ST7789 Library`, `Adafruit GFX Library`, and `PNGdec` for the firmware. `WiFi`,
`HTTPClient`, `LittleFS` and `SPI` ship with the ESP32 core.

> If you do open this in the Arduino IDE anyway (it still works, the builder is
> just slow to fail), the Tools menu needs: Board `ESP32S3 Dev Module`, USB CDC
> On Boot **Enabled**, Flash Size `4MB (32Mb)`, PSRAM `QSPI PSRAM`, Partition
> Scheme `Default 4MB with spiffs`, Upload Speed `921600`, Core Debug Level
> `None`. USB CDC On Boot is the one that silently breaks the Serial Monitor.

> `PNGdec` decoding line-by-line keeps RAM use under 4 KB, so PSRAM is a bonus
> rather than a requirement. The whole-file buffer in `CACHE_PATH` reads is
> sized for the 2 MB of PSRAM, so keep PSRAM enabled.

---

## 3. Server setup

### 3.1 Run it

```bash
docker compose up -d
```

Dashboard lands on `http://<server-ip>:9607`. On boot, create the admin account.

### 3.2 Add a photo source

Settings → Data Sources. Two good starting points:

- **Gallery** — no external setup, upload photos straight from your phone.
- **Google Photos** — needs OAuth from your Google account.

A device is assigned exactly **one** source at a time
(`Device.Source` is a single column, `backend/internal/model/model.go:98`). Since
you want both: create **one** device, and flip its **Image Source** dropdown in
the web UI whenever you want to switch between Gallery and Google Photos. The
firmware holds a single token, so it does not care which source is selected.

Click **Sync Now** once to pull the library down.

### 3.3 Add the device

Settings → Devices → Add Device:

- **Name** — anything, it gets overwritten by `/api/system-info`
- **Host** — the ESP32's IP. It does not have to be up yet: the server falls
  back to placeholder dimensions for an unreachable frame
  (`backend/internal/service/device.go:97-107`) and your first `/image` fetch
  writes the real ones via the `X-Display-*` headers
  (`backend/internal/handler/image.go:171-202`).
- **Image Source** — Gallery, or Google Photos
- **Overlays** — clock, date, weather. These are rendered *server-side* into the
  PNG, so they cost you nothing on the device.
- **Display Settings → Full Colour** — **turn this on.** See §6. Without it the
  server dithers every photo into 6 e-paper colours, because the whole image
  pipeline is built for e-paper. The firmware also reports
  `"full_colour": true` from `/api/system-info`, which turns it on for you.

> **Leave the tone / dither / palette sliders alone.** They exist to make photos
> look right on 6-colour e-paper, and on a full-colour TFT they are actively
> harmful: any value there becomes an `X-Processing-Settings` /
> `X-Color-Palette` header that tells the server which palette to dither down
> to. The firmware sends neither header, which is deliberate. With Full Colour
> on they are ignored entirely.

### 3.4 Generate a device token

Settings → Security → Device Tokens → Generate, with this device selected.

> **This must be a device token, not an admin login token.** The server resolves
> the device solely from the token's `device_id` claim
> (`backend/internal/handler/image.go:100-108`). An admin token has no
> `device_id`, so `/image` answers **400** with an empty source.

### 3.5 Verify before you flash

The firmware's exact request, runnable from a terminal:

```bash
curl -v -o /tmp/frame.png \
  -H "Authorization: Bearer YOUR_DEVICE_TOKEN" \
  -H "X-Display-Width: 160" \
  -H "X-Display-Height: 128" \
  -H "X-Display-Orientation: landscape" \
  -H "X-Firmware-Version: 1.0.0" \
  http://192.168.1.100:9607/image

file /tmp/frame.png     # must say: PNG image data, 160 x 128
```

Decode it locally to see exactly what the panel will get:

```bash
python3 -c "import struct;d=open('/tmp/frame.png','rb').read();print('size',len(d),'dims',struct.unpack('>II',d[16:24]))"
```

Interpreting the result:

| Result | Meaning |
|---|---|
| `200`, PNG, 160×128 | good — flash the firmware |
| `400` | the device has no image source assigned, or you used an admin token |
| `401` | token typo, or the key was deleted/rotated |
| `404` | the source rejected its album/topic filter, or the library is empty |
| `500` | a render or converter failure — check the server's docker logs |
| PNG but `application/octet-stream` | you sent `X-Firmware-Version` ≥ `2.6.1`; the server then returns EPDGZ e-paper data that PNGdec cannot decode. Use `1.0.0`. |

Any `2.6.1`+ or `dev-*` version string flips the response to EPDGZ. That is the
single most common cause of "the frame shows a photo but the server says
octet-stream".

---

## 4. Build order

### Gate 1 — smoke test

```bash
cd firmware
~/pio-venv/bin/pio run -e smoke -t upload
~/pio-venv/bin/pio device monitor
```

It touches no Wi-Fi, no server and no flash storage. It lights the backlight,
initialises the panel, then loops a test pattern: white, red/green/blue bars,
corner markers, a diagonal and grid, and finally a colour-order reference card
(K R G Y B M C W left to right).

Report back:

1. Serial Monitor output, in full
2. Did the backlight light up? **How bright?**
3. Is the image shifted, or are there stray coloured lines along one edge?
4. Do colours look negative (white renders black)?
5. In the reference card, is the leftmost band **black** and the rightmost **white**?
6. Is the left band red and the fifth band blue?

Nothing else can be diagnosed until these are answered.

#### Backlight brightness

With `BACKLIGHT_ON_GPIO4 false` the backlight is on the 3.3 V rail, so the
firmware has no say in its brightness at all. A dim panel here is a *hardware*
question:

| Symptom | Cause | Fix |
|---|---|---|
| Dim but clearly visible | panel's backlight LED wants more current than the module's onboard resistor allows | Feed `BLK` from a proper 3.3 V supply with a lower series resistance (the fitted part reads 330 Ω, i.e. 33 Ω, which limits the LED to about 9 mA) |
| Very dim, other tests fine | same, but the fitted resistor is larger than intended | Measure the actual resistance, fit a lower value |
| Off entirely | `BLK` not connected, or connected to 5 V and the LED is reverse-biased | Confirm the wire reaches the 3.3 V rail |

Never feed 5 V into a GPIO, and never drive `BLK` from a GPIO. A bare backlight
LED with no limiting resistor needs one added before it goes on 3.3 V.

### Gate 2 — full firmware

1. `cp esp32_s3_photoframe/secrets.h.example esp32_s3_photoframe/secrets.h`,
   then fill in the Wi-Fi credentials, the server base URL, and the device token
2. Adjust `esp32_s3_photoframe/config.h` if Gate 1 flagged anything
3. `~/pio-venv/bin/pio run -e supermini -t upload`

### Gate 3 — tuning

Only after a photo is on screen, adjust `config.h` for the display quirks below.

---

## 5. Troubleshooting

Ordered by how often each one actually bites.

| Symptom | Cause | Fix |
|---|---|---|
| No Serial Monitor output | USB CDC is off by default on the S3 | Tools → USB CDC On Boot → **Enabled** |
| Upload fails | ROM bootloader not entered | Hold **BOOT**, plug in USB, release **BOOT**. Or hold BOOT while tapping RESET. |
| White screen, no backlight | Backlight not powered | Confirm `LED/BLK → 3.3V` |
| White screen, backlight on | No SPI traffic | Check `SCK → 12`, `SDA → 11`, `CS → 10`, `A0/DC → 6`, and that grounds are common |
| `HTTP -1` or `-11`, and the server log shows the frame never requested `/image` | The static IP is not one the router considers valid — most often an unreserved address **inside** the DHCP pool | Move the static address below the pool (`.50` is used here) or add a DHCP reservation for the frame's MAC. Both verified: `.120` silently fails, DHCP works, `.50` works |
| `[probe] <gw> -> FAILED` and `[probe] <server> -> FAILED` | The frame cannot reach anything outbound, not just the server | Almost always the row above. Distinguishes "router dropped us" from "server is down" |
| Frame associates and shows a banner but never fetches a photo | Same as above — the failure is invisible without the probe line | Read the Serial Monitor; `NET_DIAG_ON_BOOT` in `config.h` prints the reachability probes |
| Frame is intermittently unreachable (server polls sometimes time out, `ping` shows loss and multi-second RTT) | Weak or noisy RF link | Read `wifi_rssi` from `GET /api/system-info` or the `X-Wifi-Signal` header in the server access log. Under about -75 dBm is marginal, under -85 dBm unreliable. Move the frame, or if it has an IPEX/u.FL antenna socket use an external antenna — the SuperMini's default PCB antenna sits right against the metal back of the panel |
| Web UI shows the device as 800x480, buttons error out | Server has not read `/api/system-info` yet | `curl http://<frame-ip>/api/system-info` should return 160x128; the UI's **Refresh** button makes the server pull it |
| Web UI config changes have no effect on the frame | `X-Config-Payload` not being read back | Response headers must be registered with `collectHeaders()` before the request; `addHeader()` does not register them. See the note in `main.cpp` |
| Frame shows a stale photo for a long time after a failure | Refresh interval is 15 min and used to apply to failures too | `SYNC_RETRY_AFTER_FAIL_MS` (60 s) retries sooner after a failure |
| Image shifted 1-3 px, stray coloured lines on one edge | Wrong panel tab for this glass revision | `TFT_TAB`: `INITR_BLACKTAB` → `INITR_REDTAB` → `INITR_GREENTAB` |
| Colours negative | Module wired for inversion | `TFT_INVERT true` |
| Reds and blues swapped | RGB565 byte order | `PNG_BIG_ENDIAN false` |
| Image rotated 90° | Wrong rotation for the ribbon | `TFT_ROTATION` 0/1/2/3 — change nothing else |
| `400` from the server | No source on the device, or an admin token was used | Settings → Devices → set Image Source; regenerate a **device** token |
| `401` from the server | Token mismatch | Re-copy it; a deleted key returns 401 "token revoked" |
| Frame shows no photo, log says `HTTP 400` at boot | Headers not sent, or the server is a different instance | Run the curl in §3.5 |
| First fetch times out, later ones work | The guide's 10 s timeout is too short for a cold render | `HTTP_TIMEOUT_MS` is already 45000; raise it to 90000 if your server is slow |
| Photo is posterised into 6 colours | The server thinks this is 6-colour e-paper | See §6 |
| Weak/flaky Wi-Fi | Known ceramic antenna issue on some SuperMini boards | Move closer to the router, or solder a 31 mm thin wire as an antenna mod |

---

## 6. The 6-colour dithering problem

This matters more than anything else in this document, so it gets its own
section.

The server's image pipeline was built for e-paper, and it is worth being precise
about what that costs this panel. The server asks the device what kind of panel
it is via `Device.DisplayType` — and that field is **only ever filled from a
`GET /api/system-info` call the server makes to the frame**
(`backend/internal/service/device.go:250-262`). The check itself is
`backend/internal/model/model.go:126`:

```go
func (d *Device) IsGrayscale() bool {
    return strings.HasPrefix(d.DisplayType, "gc") || d.BoardName == "seeedstudio_reterminal_e1003"
}
```

Only the `"gc"` prefix matters. `"spectra6"` and `""` both mean "not grayscale".

**The key fact, verified in the code:** every photo request is handed to
`epaper-image-convert`, unconditionally
(`backend/internal/handler/image.go:445-493` → `processor.go:167`).

```go
grayscale := deviceFound && device.IsGrayscale()
headerOpts := h.processor.MapProcessingSettings(settings, palette, grayscale)
processedBytes, thumbBytes, err := h.processor.ProcessImage(imgWithOverlay, procOptions)
```

`MapProcessingSettings` only adds a palette when the request carries
`X-Color-Palette` or the device has stored processing settings
(`backend/internal/service/processor.go:63-95`):

- grayscale branch → `opts["palette-preset"] = "grayscale16"`
- otherwise, only `if palette != nil` → `opts["palette"] = ...`

This firmware sends neither header, so on the normal path the converter is
invoked with **no palette at all**. That does *not* make it emit a truecolour
PNG — the converter is an e-paper tool and runs either way. Treat "the default
path might just give full colour" as unproven; it is not something to plan
around.

### The fix

This is now implemented. The pipeline already had a precedent for skipping
the converter: the generative sources `fractal` and `dla` encode straight to
PNG with Go's stdlib and never shell out. The full-colour path now does the
same for any device with the flag set:

- `devices.full_colour` — a per-device boolean, default off
- when it is set, `backend/internal/handler/image.go` resizes in-process with
  CatmullRom (matching the resize the gallery and Google Photos paths already
  use), reproduces `cover`/`fit` and the letterbox background, encodes a
  truecolour PNG and returns it without calling `ProcessImage`
- `GET /api/system-info` may report `"full_colour": true`, which turns the
  bypass on. It is deliberately one-way: a frame that omits the field never
  turns it back off, so **Settings → Devices → Display Settings → Full
  Colour** stays authoritative for switching it off

Defaults are unchanged for every existing e-paper frame.

Turn it on in the web UI, or let the firmware report it — `config.h` sets
`REPORT_FULL_COLOUR true` for this panel. Leave `DISPLAY_TYPE` at
`"spectra6"`.

`DISPLAY_TYPE "gc16"` trades colour for a 16-step grey ramp, which is better
than 6-colour dithering but still not colour. It is now a diagnostic to try
once if the bypass cannot be enabled, not a setting to leave on.

---

## 7. What the firmware does

- Boots, paints the cached photo from `LittleFS` immediately, *then* tries the
  network. The frame is never blank while it waits.
- Streams `GET /image` into a temp file, verifies the byte count against
  `Content-Length`, and only then renames over the cache. A dropped connection
  leaves the previous photo intact instead of corrupting it.
- Backs off and retries failed fetches, and leaves the cached photo alone when
  all attempts fail.
- Serves `GET /api/system-info` and `GET /api/config` on itself, so the web UI
  shows the right name and dimensions and its **Open Device** / **Refresh**
  buttons work.
- Accepts `X-Config-Payload` on image responses, letting the web UI drive the
  refresh interval, sleep schedule, timezone and NTP server without a reflash.
- Runs its main loop as a non-blocking state machine, feeding the task watchdog,
  so a slow render never freezes the display or trips a reset.

## 8. Files

| File | Purpose |
|---|---|
| `platformio.ini` | Build config for both envs. Pins/settings live here, not in an IDE. |
| `boards/esp32-s3-supermini.json` | SuperMini board definition: 4 MB flash, QSPI PSRAM, 4 MB partitions |
| `src/smoke.cpp` | Gate 1. Wiring proof. No Wi-Fi, no flash writes. |
| `src/main.cpp` | The firmware. |
| `esp32_s3_photoframe/config.h` | Pins, geometry, timing, feature flags. No secrets. |
| `esp32_s3_photoframe/secrets.h.example` | Template. Copy to `secrets.h` — which is gitignored. |
