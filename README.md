# RoboEyes

Two pill-shaped eyes for the **Waveshare ESP32-S3-Touch-AMOLED-1.43** (466×466 round AMOLED, capacitive touch), plus a web app to color them.

The round screen itself is the character's face. It's rendered as a 3D ball the exact size of the display, so only the eyes are drawn, and they squash and lean naturally as the face looks around.

- **On its own:** white face, black eyes (or your colors). Both eyes blink together and keep gliding around.
- **Touch:** the face and eye colors **swap** with a circular wipe, and the eyes **smile** (arches, looking at you). The smile lasts while your finger is down plus 2.5 s after it lifts, the same as EyeBuddy's touch smile. Then the usual pill eyes come back, and the swapped colors stay.
- **Web app:** **https://softobsessing.github.io/RoboEyes/** picks the face and eye colors live, saves color pairs, and mirrors touches in a live preview.

![preview](firmware/RoboEyes/extras/preview/preview.png)

_A touch: white face → black face with white eyes, smiling, then back to the usual pills (still swapped)._

Works with both panel variants Waveshare ships on this board (SH8601 and CO5300). The panel is detected at boot.

## Repo layout

| Path | What |
|---|---|
| `index.html`, `style.css`, `app.js` | the web app (served by GitHub Pages from the repo root) |
| `roboeyes.js` | line-for-line JavaScript port of the board's eye code, for the live preview |
| `link.js` | USB (Web Serial) and Bluetooth (Web Bluetooth, Nordic UART) link |
| `board.js` | recognizing a RoboEyes board (`HELLO`), color lines |
| `colors.js` | the color wheel + saved colors |
| `firmware/RoboEyes/` | the Arduino sketch |
| `test/` | `node` tests: protocol, touch behavior, and preview-vs-firmware pixel parity |

## The web app

Open **https://softobsessing.github.io/RoboEyes/** in desktop **Chrome or Edge**. They're the browsers that can talk to USB and Bluetooth.

1. **Connect USB** (plug the board in), or **Connect Bluetooth** and pick `RoboEyes-XXXX`. The page picks up the colors the board is showing.
2. **Colors:** choose **Face** or **Eyes**, then drag on the wheel, use the brightness slider, or type a hex code. The board changes as you drag and remembers the colors after it's switched off. **Save these colors** keeps the pair in this browser. Click a saved face to switch back.
3. **Touch the board:** the colors swap and it smiles. The page's preview and color picker follow. You can also click the face in the preview to do the same (on the board too, if one is connected).

After the first time, the page reconnects by itself: USB on page load or when the board is plugged back in, Bluetooth if Chrome allows it. The Bluetooth button remembers your board (`Connect RoboEyes-3F2A`), and **Other board…** picks a different one.

**Each app talks only to its own boards.** The RoboEyes app only drives boards that say `HELLO RoboEyes`. The [doomscroll-eyes app](https://softobsessing.github.io/doomscroll-eyes/) only drives EyeBuddy boards. Both boards use the same USB chip, so if you connect one to the wrong app, the page tells you which app it belongs to, links there, and leaves the port alone. Bluetooth choosers are filtered by name (`RoboEyes-…` vs `EyeBuddy-…`), so the wrong boards never show up there.

### Run it locally

```sh
./serve.sh     # then open http://localhost:8000/ in Chrome
```

(`localhost` counts as a secure context, so USB and Bluetooth work.)

### Publish it (GitHub Pages)

On GitHub: **Settings → Pages → Build and deployment → Deploy from a branch → `main` / `(root)` → Save**. After a minute the app is at `https://softobsessing.github.io/RoboEyes/`. GitHub Pages on a free account needs the repo to be **public**.

## Flashing the firmware

The sketch lives in **`firmware/RoboEyes/`**.

### Arduino IDE

1. Install the board package **"esp32 by Espressif Systems"** (v3.1 or newer). No other libraries are needed.
2. Open `firmware/RoboEyes/RoboEyes.ino`, keeping every file in that folder together.
3. Tools menu:
   | Setting | Value |
   |---|---|
   | Board | ESP32S3 Dev Module |
   | USB CDC On Boot | Enabled |
   | Flash Size | 16MB |
   | PSRAM | OPI PSRAM |
4. Plug in the board and click **Upload**.

### arduino-cli

```sh
cd firmware
arduino-cli compile --upload -p /dev/cu.usbmodem* \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi" RoboEyes
```

Close the Arduino Serial Monitor before connecting the web app over USB: only one program can use the port at a time.

## Customizing

Everything you're likely to change is at the top of `firmware/RoboEyes/RoboEyes.ino`:

| Setting | What it does |
|---|---|
| `DEFAULT_FACE`, `DEFAULT_EYES` | Colors until the web app sets others (`0xRRGGBB`) |
| `SMILE_MS` | How long the touch smile lasts after the finger lifts (2500, like EyeBuddy) |
| `WIPE_MS` | Speed of the color swap |
| `FACE_RADIUS` | Size of the ball the eyes sit on (236 = fills the screen; bigger = bigger eyes) |
| `HOME_YAW`, `HOME_PITCH`, `HOME_ROLL` | Resting gaze direction (default: up and to the right, like the reference art) |
| `GAZE_WANDER`, `GAZE_DRIFT` | How much the eyes look around / float while idle |
| `GAZE_SPEED` | Eye movement speed (`1.0` default; lower = slower and silkier) |
| `EYE_SPACING`, `EYE_HEIGHT`, `EYE_WIDTH`, `EYE_LENGTH`, `EYE_TILT` | Eye shape and placement |
| `SCREEN_ROTATION` | Turns the picture clockwise: `0`, `90` (default), `180`, `270` |
| `BRIGHTNESS` | `0`–`255` |
| `TARGET_FPS` | Frame-rate cap (default 60) |
| `SHOW_FPS` | `1` = print `D fps …` to the Serial Monitor (115200 baud) every 10 s |
| `BLE_NAME` | Bluetooth name prefix; keep `RoboEyes` (the web app looks for it) |

If you change shapes, moods, colors or timings in `eyes_anim.h` / `eyes_render.h`, make the same change in `roboeyes.js` and run the tests below, so the web preview keeps matching the board.

### Previewing on a computer

`eyes_render.h` and `eyes_anim.h` are plain C++, so you can preview changes without flashing:

```sh
cd firmware/RoboEyes
clang++ -O2 -std=c++17 -o /tmp/preview extras/preview/preview.cpp
/tmp/preview sheet.ppm 2000                         # frames every 50 ms, a touch at 2.0 s
STEP_MS=100 FIRST_MS=3000 /tmp/preview sheet.ppm 2000   # later and slower: the smile ending
```

(The preview uses its own copy of the settings at the top of `preview.cpp`.)

## Tests

From the repo root (Node 18+ and clang):

```sh
node test/board.test.mjs     # HELLO parsing, wrong-board detection, color lines
node test/touch.test.mjs     # the touch rule: swap, smile for held + 2.5 s, back to pills, colors kept
clang++ -O2 -std=c++17 -o /tmp/robo_parity firmware/RoboEyes/extras/parity/parity.cpp
mkdir -p /tmp/robo_parity_out && /tmp/robo_parity /tmp/robo_parity_out
node test/roboeyes_port.test.mjs /tmp/robo_parity_out   # web preview vs firmware, pixel by pixel
```

## Protocol

Text lines, `\n`-terminated, the same over USB serial (115200) and Bluetooth (Nordic UART, name `RoboEyes-<ID>`, where the ID is the last 2 bytes of the chip's MAC address).

| Direction | Line | Meaning |
|---|---|---|
| app → board | `P` | hello? (sent on connect) |
| app → board | `K <face RRGGBB> <eyes RRGGBB>` | set the colors now (saved on the board 1.5 s after the last change) |
| app → board | `TAP` | do what a touch does: swap colors + smile |
| app → board | `E <neutral\|happy\|side\|sad> <lookX> <lookY>` | drive the mood + gaze (−100..100). Not used by the app yet. The board goes back to doing its own thing 3 s after the last `E` |
| app → board | `B` / `B2` | blink / double blink (only while `E` lines keep coming) |
| board → app | `HELLO RoboEyes <protocol> <ID> <firmware>` | e.g. `HELLO RoboEyes 2 3F2A 2`; on boot and in reply to `P` |
| board → app | `C <face RRGGBB> <eyes RRGGBB>` | its colors: after `HELLO` and at every swap |
| board → app | `T touch` / `T release` | finger down / up |
| board → app | `D fps <n> draw_ms <n>` | USB only, every 10 s (frame rate; the app ignores it) |

Moods (`E` lines), for reference:

![moods](firmware/RoboEyes/extras/preview/moods.png)

_neutral, happy, side-eye, disappointed. Top: looking straight ahead. Bottom: looking up and to the right._

## How it stays smooth

Every frame is generated live, so smoothness comes down to frames per second and how far the eyes move between frames:

- **Only changed pixels are redrawn.** The face is one solid color, so each frame redraws just the area around each eye. During the color swap it adds only the thin ring the wipe circle grew through since the last frame (`eyes_dirty.h`). A new color from the web app redraws the screen once.
- **The 3D math is limited to a tight box around each eye**, traced from the pill's outline.
- **Both CPU cores draw**, and DMA sends one strip to the panel while the next is drawn.
- **Touch is read on a background task**, so it can never delay a frame.
- **Motion eases in and out.** Eye movement goes through two smoothing stages, so speed rises and falls gently, and a new target mid-move blends in without a jerk.

## Hardware pins

| Signal | GPIO |
|---|---|
| QSPI CS / CLK | 9 / 10 |
| QSPI D0–D3 | 11, 12, 13, 14 |
| AMOLED reset | 21 |
| AMOLED power enable | 42 |
| Touch SDA / SCL (FT3168 @ 0x38) | 47 / 48 |

## Credits

- `esp_lcd_sh8601.c/.h`: Espressif Systems, Apache-2.0, as bundled in Waveshare's demo package.
- `ble_link.cpp/.h`, `link.js`, `colors.js`: from the EyeBuddy firmware and web app in the doomscroll-eyes repo.
- `read_lcd_id_bsp.c/.h`: from Waveshare's [ESP32-S3-Touch-AMOLED-1.43 demo](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43).
