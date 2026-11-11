# RoboEyes

Animated robot eyes for the **Waveshare ESP32-S3-Touch-AMOLED-1.43** (466×466 round AMOLED, capacitive touch).

- **Idle:** blue eyes that blink and glance around.
- **Tap the screen:** the eyes change to a second expression and fade to orange.
- **Tap again:** back to idle (or set `AUTO_RETURN_MS` to return automatically).

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
| `IDLE_COLOR`, `TOUCHED_COLOR` | Eye colors (`0xRRGGBB`) |
| `IDLE_SHAPE`, `TOUCHED_SHAPE` | Size, roundness, spacing, upper-lid slant (angry/sad), lower-lid curve (happy) |
| `AUTO_RETURN_MS` | `0` = tap toggles; otherwise return to idle after N ms |
| `MORPH_SPEED` | How quickly the expressions blend |
| `SCREEN_ROTATION` | `0`, `90`, `180`, `270` if the image is sideways |
| `BRIGHTNESS` | `0`–`255` |

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
