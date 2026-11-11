# RoboEyes

A little ball character with two pill-shaped eyes for the **Waveshare ESP32-S3-Touch-AMOLED-1.43** (466×466 round AMOLED, capacitive touch).

- **Idle:** white ball on a black screen. The eyes are cut-outs, both blink together, and the ball floats and glances around.
- **Tap:** the ball looks around and the screen inverts with a circular wipe: white screen, black ball.
- **Tap again:** back to black (or set `AUTO_RETURN_MS` to go back automatically).

The ball is rendered as a real 3D sphere, so the eyes squash and lean naturally as it looks toward its edge.

![preview](extras/preview/preview.png)

Works with both panel variants Waveshare ships on this board (SH8601 and CO5300). The panel is detected at boot.

## Flashing (Arduino IDE)

1. Install the board package **"esp32 by Espressif Systems"** (v3.1 or newer). No other libraries are needed.
2. Open `RoboEyes.ino`, keeping every file in this folder together.
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
arduino-cli compile --upload -p /dev/cu.usbmodem* \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi" .
```

## Customizing

Everything you're likely to change is at the top of `RoboEyes.ino`:

| Setting | What it does |
|---|---|
| `IDLE_BACKGROUND`, `IDLE_BALL` | Colors before tapping (`0xRRGGBB`) |
| `TAP_BACKGROUND`, `TAP_BALL` | Colors after tapping |
| `BALL_RADIUS` | Ball size in pixels (screen is 466 wide) |
| `HOME_YAW`, `HOME_PITCH`, `HOME_ROLL` | Resting gaze direction (default: up and to the right, like the reference art) |
| `GAZE_WANDER`, `BALL_DRIFT` | How much it looks around / floats while idle |
| `EYE_SPACING`, `EYE_HEIGHT`, `EYE_WIDTH`, `EYE_LENGTH`, `EYE_TILT` | Eye shape and placement |
| `WIPE_MS` | Speed of the black/white inversion |
| `AUTO_RETURN_MS` | `0` = tap toggles; otherwise return to black after N ms |
| `SCREEN_ROTATION` | `0`, `90`, `180`, `270` if the image is sideways |
| `BRIGHTNESS` | `0`–`255` |

### Previewing on a computer

`eyes_render.h` and `eyes_anim.h` are plain C++, so you can preview changes without flashing:

```sh
clang++ -O2 -std=c++17 -o preview extras/preview/preview.cpp
./preview sheet.ppm 1900 3000     # frames every 50 ms, taps at 1.9 s and 3.0 s
```

(The preview uses its own copy of the settings at the top of `preview.cpp`.)

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
- `read_lcd_id_bsp.c/.h`: from Waveshare's [ESP32-S3-Touch-AMOLED-1.43 demo](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43).
