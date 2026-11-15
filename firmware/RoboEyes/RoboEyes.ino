/*
 * RoboEyes - two pill-shaped eyes for the Waveshare ESP32-S3-Touch-AMOLED-1.43
 * (466x466 round AMOLED, FT3168 touch).
 *
 * The round screen itself is the character's face: it's rendered as a 3D ball
 * exactly the size of the display, so only the eyes are drawn on it.
 *
 * - On its own: white face, black eyes (or your colors). Both eyes blink
 *   together and glide around.
 * - Touch: the face and eye colors swap with a circular wipe. The eyes stay
 *   the same pills; touch again to swap back.
 * - Blink with you: with the web app's camera on, it blinks when you blink.
 * - Touch can instead make the eyes follow your finger (pick it in the web app;
 *   the board remembers). From the web app: dart left / right, and shake (the
 *   eyes bounce all around the screen, squashing as they hit the edge), and
 *   heart eyes (the pills melt into beating hearts).
 *
 * Web app: https://softobsessing.github.io/RoboEyes/ connects over USB or
 * Bluetooth ("RoboEyes-XXXX") to pick the face and eye colors live and save
 * them. The board remembers its colors when switched off. It only talks to
 * the RoboEyes app; the doomscroll-eyes (EyeBuddy) app won't drive it.
 *
 * Every frame is generated live, so smoothness = frames per second. To keep
 * that high, only the pixels that changed are redrawn, both CPU cores share
 * the drawing, and touch is read on a background task. Open the Serial
 * Monitor (115200) to see the frame rate.
 *
 * Arduino IDE settings (Tools menu):
 *   Board:            ESP32S3 Dev Module
 *   USB CDC On Boot:  Enabled
 *   CPU Frequency:    240MHz (WiFi)
 *   Flash Size:       16MB
 *   PSRAM:            OPI PSRAM
 * Board package: "esp32 by Espressif Systems" 3.1.x or newer. No extra libraries needed.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_mac.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "esp_lcd_sh8601.h"
#include "read_lcd_id_bsp.h"
#include "eyes_render.h"
#include "eyes_anim.h"
#include "eyes_dirty.h"
#include "ble_link.h"

// =====================================================================
//  LOOK & FEEL - tweak these
// =====================================================================

// Starting colors as 0xRRGGBB (the web app can change them; the board remembers)
#define DEFAULT_FACE      0xFFFFFF   // white screen...
#define DEFAULT_EYES      0x000000   // ...black eyes

// Size of the 3D ball the eyes sit on. 236 = just past the 233 px screen
// radius, so the ball's edge is the screen's edge. Bigger = bigger eyes that
// move further across the screen.
#define FACE_RADIUS       236

// Resting gaze, matched to the reference images (looking up and to the right)
#define HOME_YAW          28         // degrees, + = right
#define HOME_PITCH        27         // degrees, + = up
#define HOME_ROLL         -10        // degrees, + = clockwise tilt

#define GAZE_WANDER       10         // degrees the gaze wanders around while idle
#define GAZE_DRIFT        4          // degrees of slow continuous floating
#define GAZE_SPEED        1.0f       // eye movement speed: lower = slower and silkier
#define WIPE_MS           650        // speed of the color swap when touched

// What touching the screen does until the web app picks: 0 = swap the colors,
// 1 = the eyes follow your finger. (The board remembers the web app's choice.)
#define TOUCH_MODE        0
// If the eyes follow your finger to the wrong spot (mirrored, or turned), fix
// the touch panel's axes here: swap X/Y first, then flip.
#define TOUCH_SWAP_XY     0
#define TOUCH_FLIP_X      0
#define TOUCH_FLIP_Y      0

// Eye shape, in fractions of the ball radius
#define EYE_SPACING       0.25f      // half the distance between the eyes
#define EYE_HEIGHT        0.00f      // + = lower on the face
#define EYE_WIDTH         0.20f      // pill width
#define EYE_LENGTH        0.42f      // pill length (tip to tip)
#define EYE_TILT          0          // degrees, leans the pill tops outward

#define SCREEN_ROTATION   90         // turns the picture clockwise: 0, 90, 180, 270
#define BRIGHTNESS        255        // 0..255
#define TARGET_FPS        60         // frame-rate cap
#define SHOW_FPS          1          // every 10 s print "D fps ..." to the Serial Monitor

// Web app link. Bluetooth name = BLE_NAME-<board ID>, e.g. "RoboEyes-3F2A"; keep "RoboEyes",
// that's what the RoboEyes web app looks for.
#define BLE_NAME          "RoboEyes"
#define FW_VERSION        4          // shown in the web app (3: blinks with you; 4: follow finger, dart, shake, hearts)
#define FOLLOW_YAW        40         // degrees the eyes turn when the website looks fully left/right
#define FOLLOW_PITCH      30         // degrees the eyes turn when the website looks fully up/down
#define LINK_TIMEOUT_MS   3000       // no message from the website this long = back to standalone

// =====================================================================
//  BOARD PINS (Waveshare ESP32-S3-Touch-AMOLED-1.43) - don't change
// =====================================================================
#define LCD_SIZE     466
#define PIN_LCD_CS   9
#define PIN_LCD_CLK  10
#define PIN_LCD_D0   11
#define PIN_LCD_D1   12
#define PIN_LCD_D2   13
#define PIN_LCD_D3   14
#define PIN_LCD_RST  21
#define PIN_LCD_PWR  42
#define PIN_TP_SDA   47
#define PIN_TP_SCL   48
#define FT3168_ADDR  0x38

#define SH8601_ID    0x86
#define BUF_PIXELS   (LCD_SIZE * 32)   // pixels per DMA buffer (two buffers)

// =====================================================================
//  Display driver (uses Waveshare's esp_lcd_sh8601 driver)
// =====================================================================
static esp_lcd_panel_handle_t panel = nullptr;
static esp_lcd_panel_io_handle_t panelIo = nullptr;
static int xGap = 0;                       // CO5300 panels need a 6 px column offset
static uint16_t *dmaBuf[2];
static SemaphoreHandle_t bufFree;          // counts DMA buffers free to draw into

static const sh8601_lcd_init_cmd_t sh8601_init[] = {
  {0x11, (uint8_t[]){0x00}, 0, 120},
  {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
  {0x35, (uint8_t[]){0x00}, 1, 0},
  {0x53, (uint8_t[]){0x20}, 1, 10},
  {0x51, (uint8_t[]){0x00}, 1, 10},
  {0x29, (uint8_t[]){0x00}, 0, 10},
};
static const sh8601_lcd_init_cmd_t co5300_init[] = {
  {0x11, (uint8_t[]){0x00}, 0, 80},
  {0xC4, (uint8_t[]){0x80}, 1, 0},
  {0x53, (uint8_t[]){0x20}, 1, 1},
  {0x63, (uint8_t[]){0xFF}, 1, 1},
  {0x51, (uint8_t[]){0x00}, 1, 1},
  {0x29, (uint8_t[]){0x00}, 0, 10},
};

static bool IRAM_ATTR onTransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(bufFree, &woken);
  return woken == pdTRUE;
}

static void setBrightness(uint8_t level) {
  // 0x51 = display brightness; QSPI command format is 0x02 00 CMD 00
  uint32_t cmd = (0x02UL << 24) | (0x51UL << 8);
  esp_lcd_panel_io_tx_param(panelIo, cmd, &level, 1);
}

static void displayInit() {
  pinMode(PIN_LCD_PWR, OUTPUT);
  digitalWrite(PIN_LCD_PWR, HIGH);
  delay(20);

  uint8_t id = read_lcd_id();
  bool isSH8601 = (id == SH8601_ID);
  xGap = isSH8601 ? 0 : 6;
  Serial.printf("Panel ID 0x%02X -> %s\n", id, isSH8601 ? "SH8601" : "CO5300");

  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_LCD_CLK;
  bus.data0_io_num = PIN_LCD_D0;
  bus.data1_io_num = PIN_LCD_D1;
  bus.data2_io_num = PIN_LCD_D2;
  bus.data3_io_num = PIN_LCD_D3;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = BUF_PIXELS * 2 + 64;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = PIN_LCD_CS;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  io.pclk_hz = 40 * 1000 * 1000;
  io.trans_queue_depth = 4;
  io.on_color_trans_done = onTransferDone;
  io.user_ctx = nullptr;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = 1;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &panelIo));

  static sh8601_vendor_config_t vendor = {};
  vendor.init_cmds = isSH8601 ? sh8601_init : co5300_init;
  vendor.init_cmds_size = isSH8601 ? sizeof(sh8601_init) / sizeof(sh8601_init[0])
                                   : sizeof(co5300_init) / sizeof(co5300_init[0]);
  vendor.flags.use_qspi_interface = 1;

  esp_lcd_panel_dev_config_t cfg = {};
  cfg.reset_gpio_num = PIN_LCD_RST;
  cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  cfg.bits_per_pixel = 16;
  cfg.vendor_config = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panelIo, &cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

  for (int i = 0; i < 2; i++) {
    dmaBuf[i] = (uint16_t *)heap_caps_malloc(BUF_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(dmaBuf[i]);
  }
  bufFree = xSemaphoreCreateCounting(2, 2);
}

// =====================================================================
//  Touch (FT3168) - polled on its own task so it can never stall a frame
// =====================================================================
static volatile bool touchDownPending = false, touchUpPending = false;
// Latest finger position, in one word so the frame loop always reads a matching
// pair: bit 31 = finger down, bits 16..27 = x, bits 0..11 = y (panel pixels).
static volatile uint32_t touchState = 0;
static const uint32_t TOUCH_DOWN_BIT = 0x80000000u;

// First touch point: count at 0x02, then X high/low at 0x03/0x04, Y at 0x05/0x06.
static bool readTouch(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(FT3168_ADDR, 5) != 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = Wire.read();
  uint8_t n = b[0] & 0x0F;
  if (n == 0 || n > 5) return false;
  x = ((b[1] & 0x0F) << 8) | b[2];
  y = ((b[3] & 0x0F) << 8) | b[4];
  return true;
}

static void touchTask(void *) {
  bool down = false;
  uint8_t misses = 0;
  for (;;) {
    uint16_t x, y;
    if (readTouch(x, y)) {
      misses = 0;
      touchState = TOUCH_DOWN_BIT | ((uint32_t)(x & 0xFFF) << 16) | (y & 0xFFF);
      if (!down) { down = true; touchDownPending = true; }
    } else if (down && ++misses >= 2) {   // two empty reads in a row = lifted (skips one-off glitches)
      down = false;
      touchState = touchState & ~TOUCH_DOWN_BIT;   // keep the last spot
      touchUpPending = true;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

static void touchInit() {
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
  Wire.setTimeOut(10);
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0x00);   // device mode register
  Wire.write(0x00);   // normal operating mode
  Wire.endTransmission();
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 1, nullptr, 0);
}

// =====================================================================
//  Drawing - both CPU cores render each strip, DMA sends while we draw the next
// =====================================================================
static eyes::Animator anim;
static eyes::Xform xform;
static eyes::DirtyTracker dirty;

struct RenderJob {
  const eyes::Scene *s;
  const eyes::Prepared *p;
  uint16_t *buf;
  int x0, y0, w, rows;
};
static RenderJob job;
static TaskHandle_t workerHandle = nullptr, loopHandle = nullptr;

static void renderWorker(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    eyes::renderRect(*job.s, *job.p, xform, job.buf, job.x0, job.y0, job.w, job.rows);
    xTaskNotifyGive(loopHandle);
  }
}

// Render one strip; the bottom half is drawn on core 0 at the same time.
static void renderStrip(const eyes::Scene &s, const eyes::Prepared &p,
                        uint16_t *buf, int x0, int y0, int w, int rows) {
  if (!workerHandle || rows < 4) {
    eyes::renderRect(s, p, xform, buf, x0, y0, w, rows);
    return;
  }
  int top = rows / 2;
  job = { &s, &p, buf + top * w, x0, y0 + top, w, rows - top };
  xTaskNotifyGive(workerHandle);
  eyes::renderRect(s, p, xform, buf, x0, y0, w, top);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// Render and send an aligned rect, in strips, double-buffered over DMA.
static void pushRect(const eyes::Scene &s, const eyes::Prepared &p, const eyes::Rect &r) {
  int w = r.x1 - r.x0 + 1;
  int maxRows = (BUF_PIXELS / w) & ~1;   // narrow rects get tall strips
  static int which = 0;
  for (int y = r.y0; y <= r.y1; y += maxRows) {
    int rows = min(maxRows, r.y1 - y + 1);
    xSemaphoreTake(bufFree, portMAX_DELAY);
    renderStrip(s, p, dmaBuf[which], r.x0, y, w, rows);
    esp_lcd_panel_draw_bitmap(panel, r.x0 + xGap, y, r.x1 + 1 + xGap, y + rows, dmaBuf[which]);
    which ^= 1;
  }
}

// =====================================================================
//  Web app link (same text protocol over USB serial and Bluetooth)
//   app -> board   P                       hello? (sent on connect)
//                  K <face RRGGBB> <eyes RRGGBB>   set the colors now (dragging the color wheel)
//                  TAP                     do what a touch does (swap the colors)
//                  E <neutral|happy|side|sad> <lookX -100..100> <lookY -100..100>   drive mood + gaze
//                  B                       blink now (the web app's camera saw you blink)
//                  B2                      double blink (only while E lines keep coming)
//                  M swap | M follow       what touching the screen does (saved on the board)
//                  F <x> <y> | F up        eyes follow a finger dragged on the web app's preview
//                                          (x, y: -100..100 = ball radius, from the screen center)
//                  DART L | DART R         quick glance to the screen's left / right
//                  SHAKE <seed>            bounce all around the screen (same seed = same bounces)
//                  H 1 | H 0               heart eyes on / off
//   board -> app   HELLO RoboEyes <protocol> <board ID> <firmware>   e.g. "HELLO RoboEyes 2 3F2A 4"
//                  C <face RRGGBB> <eyes RRGGBB>   current colors (after HELLO and after every swap)
//                  T touch | T release     (touch = swap mode)
//                  F <x> <y> | F up        the finger, while the eyes follow it (follow mode)
//                  M swap | M follow       touch mode (after HELLO and whenever it changes)
//                  H 1 | H 0               heart eyes (after HELLO and whenever they change)
//                  D fps <n> draw_ms <n>   (USB only, every 10 s; the app ignores it)
// =====================================================================
#define PROTOCOL 2
static char boardId[8] = "0000";
static Preferences prefs;   // NVS "roboeyes": the colors, so they survive power-off
static uint32_t savedFace, savedEyes, colorsChangedAt = 0;
static bool colorsDirty = false;

static uint8_t touchMode = TOUCH_MODE;   // 0 = swap colors, 1 = eyes follow the finger
static bool swapHeld = false, fingerFollowing = false, appFollowing = false;
static uint32_t lastSwapMs = 0, lastFollowEmitMs = 0, appFollowMs = 0;
static int lastFx = 9999, lastFy = 9999;

static bool haveLink = false;
static uint32_t lastLinkMs = 0;
static eyes::Mood linkMood = eyes::NEUTRAL;
static float linkX = 0, linkY = 0;

static void emit(const char *line) {
  Serial.println(line);
  ble_send_line(line);
}

static void emitHello() {
  char line[48];
  snprintf(line, sizeof line, "HELLO RoboEyes %d %s %d", PROTOCOL, boardId, FW_VERSION);
  emit(line);
}

static void emitColors() {
  char line[24];
  snprintf(line, sizeof line, "C %06lX %06lX", (unsigned long)anim.faceColor(), (unsigned long)anim.eyeColor());
  emit(line);
}

// Save 1.5 s after the last change, so dragging the color wheel doesn't wear out the flash.
static void saveColorsWhenSettled(uint32_t now) {
  uint32_t f = anim.faceColor(), e = anim.eyeColor();
  if (f != savedFace || e != savedEyes) {
    savedFace = f; savedEyes = e;
    colorsChangedAt = now;
    colorsDirty = true;
  }
  if (colorsDirty && now - colorsChangedAt > 1500) {
    prefs.putUInt("face", savedFace);
    prefs.putUInt("eyes", savedEyes);
    colorsDirty = false;
  }
}

static void emitMode() { emit(touchMode ? "M follow" : "M swap"); }
static void emitHearts() { emit(anim.heartsOn() ? "H 1" : "H 0"); }

static void setTouchMode(uint8_t m) {
  if (m != touchMode) {
    touchMode = m;
    prefs.putUChar("touch", m);
    if (fingerFollowing) { fingerFollowing = false; anim.followEnd(millis()); }
  }
  emitMode();
}

// A panel pixel -> ball radii from the screen center, the way the picture is turned.
static void touchToFace(uint32_t st, float &x, float &y) {
  float X = (float)((st >> 16) & 0xFFF), Y = (float)(st & 0xFFF);
  if (TOUCH_SWAP_XY) { float t = X; X = Y; Y = t; }
  if (TOUCH_FLIP_X) X = LCD_SIZE - 1 - X;
  if (TOUCH_FLIP_Y) Y = LCD_SIZE - 1 - Y;
  X += 0.5f; Y += 0.5f;
  float lx = xform.ox + xform.xx * X + xform.xy * Y;
  float ly = xform.oy + xform.yx * X + xform.yy * Y;
  x = (lx - LCD_SIZE * 0.5f) / FACE_RADIUS;
  y = (ly - LCD_SIZE * 0.5f) / FACE_RADIUS;
}

// Tell the web app where the finger is (so its preview follows too), ~25 times a second.
static void emitFollow(float x, float y, uint32_t now, bool force) {
  int ix = (int)lroundf(x * 100.f), iy = (int)lroundf(y * 100.f);
  ix = ix < -100 ? -100 : (ix > 100 ? 100 : ix);
  iy = iy < -100 ? -100 : (iy > 100 ? 100 : iy);
  if (!force && (now - lastFollowEmitMs < 40 || (ix == lastFx && iy == lastFy))) return;
  lastFollowEmitMs = now; lastFx = ix; lastFy = iy;
  char line[24];
  snprintf(line, sizeof line, "F %d %d", ix, iy);
  emit(line);
}

// A finger down / up on the screen (or the web app's TAP).
static void onTouch(bool down) {
  anim.touch(down);
  emit(down ? "T touch" : "T release");
  if (down) emitColors();   // the colors it's swapping to
}

static bool linked(uint32_t now) { return haveLink && now - lastLinkMs < LINK_TIMEOUT_MS; }

static void onLine(const char *line, uint32_t now) {
  if (line[0] == 'P' && line[1] == 0) {
    emitHello();
    emitColors();
    emitMode();
    emitHearts();
    return;
  }
  if (strcmp(line, "H 1") == 0 || strcmp(line, "H 0") == 0) {
    anim.setHearts(line[2] == '1', now);
    emitHearts();
    return;
  }
  if (strcmp(line, "M swap") == 0 || strcmp(line, "M follow") == 0) {
    setTouchMode(line[2] == 'f' ? 1 : 0);
    return;
  }
  if (strcmp(line, "F up") == 0) {
    if (appFollowing) { appFollowing = false; if (!fingerFollowing) anim.followEnd(now); }
    return;
  }
  int fx = 0, fy = 0;
  if (line[0] == 'F' && sscanf(line, "F %d %d", &fx, &fy) == 2) {
    if (!fingerFollowing) {   // a finger on the board itself wins
      fx = fx < -100 ? -100 : (fx > 100 ? 100 : fx);
      fy = fy < -100 ? -100 : (fy > 100 ? 100 : fy);
      anim.follow(fx / 100.f, fy / 100.f, now);
      appFollowing = true;
      appFollowMs = now;
    }
    return;
  }
  if (strcmp(line, "DART L") == 0 || strcmp(line, "DART R") == 0) {
    anim.dart(line[5] == 'L' ? -1 : 1, now);
    return;
  }
  unsigned long seed = 0;
  if (sscanf(line, "SHAKE %lu", &seed) == 1) {
    anim.shake((uint32_t)seed, now);
    return;
  }
  unsigned face = 0, eyes = 0;
  if (line[0] == 'K' && sscanf(line, "K %6x %6x", &face, &eyes) == 2) {
    anim.setColors(face, eyes);
    return;
  }
  if (strcmp(line, "TAP") == 0) {
    onTouch(true);
    onTouch(false);
    return;
  }
  if (strcmp(line, "B") == 0) {   // the web app saw you blink: blink with you, right now
    anim.mirrorBlink(now);
    return;
  }
  if (strcmp(line, "B2") == 0) {
    if (linked(now)) anim.blinkNow(now, true);
    return;
  }
  char mood[16];
  int x = 0, y = 0;
  eyes::Mood m;
  if (sscanf(line, "E %15s %d %d", mood, &x, &y) >= 1 && eyes::parseMood(mood, &m)) {
    if (!linked(now)) anim.blinkNow(now, false);   // the website just connected: blink hello
    haveLink = true;
    lastLinkMs = now;
    linkMood = m;
    linkX = (x < -100 ? -100 : x > 100 ? 100 : x) / 100.f;
    linkY = (y < -100 ? -100 : y > 100 ? 100 : y) / 100.f;
  }
}

struct LineReader {
  char buf[96];
  size_t len = 0;
  void feed(char c, uint32_t now) {
    if (c == '\n' || c == '\r') {
      if (len) { buf[len] = 0; onLine(buf, now); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;   // garbage / overlong line: drop it
    }
  }
};
static LineReader usbLine, bleLine;

static void pollLinks(uint32_t now) {
  while (Serial.available() > 0) usbLine.feed((char)Serial.read(), now);
  char tmp[64];
  size_t n;
  while ((n = ble_read(tmp, sizeof tmp)) > 0)
    for (size_t i = 0; i < n; i++) bleLine.feed(tmp[i], now);
}

// Touch: swap the colors, or (follow mode) the eyes follow the finger. Either
// way the web app hears about it, so its preview and color picker follow.
static void handleTouch(uint32_t now) {
  bool down = false, up = false;
  if (touchDownPending) { touchDownPending = false; down = true; }
  if (touchUpPending) { touchUpPending = false; up = true; }
  uint32_t st = touchState;
  if (touchMode == 0) {
    if (down && now - lastSwapMs > 250) { lastSwapMs = now; swapHeld = true; onTouch(true); }
    if (up && swapHeld) { swapHeld = false; onTouch(false); }
    return;
  }
  if ((st & TOUCH_DOWN_BIT) || down) {   // (a quick tap may already be over: still go there)
    float x, y;
    touchToFace(st, x, y);
    anim.follow(x, y, now);
    if (!fingerFollowing) appFollowing = false;
    fingerFollowing = true;
    emitFollow(x, y, now, down);
  } else if (fingerFollowing) {
    fingerFollowing = false;
    anim.followEnd(now);
    emit("F up");
  }
  // the web app stopped sending (closed mid-drag?): let go
  if (appFollowing && now - appFollowMs > 1500) { appFollowing = false; anim.followEnd(now); }
}

// =====================================================================
//  Arduino entry points
// =====================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never stall the animation when no computer is listening
  delay(200);
  displayInit();
  touchInit();
  setBrightness(BRIGHTNESS);

  loopHandle = xTaskGetCurrentTaskHandle();
  xTaskCreatePinnedToCore(renderWorker, "render", 4096, nullptr, 3, &workerHandle, 0);

  eyes::AnimConfig cfg;
  prefs.begin("roboeyes", false);
  savedFace = prefs.getUInt("face", DEFAULT_FACE) & 0xFFFFFF;
  savedEyes = prefs.getUInt("eyes", DEFAULT_EYES) & 0xFFFFFF;
  touchMode = prefs.getUChar("touch", TOUCH_MODE) ? 1 : 0;
  cfg.face = savedFace;
  cfg.eyes = savedEyes;
  cfg.faceRadius = FACE_RADIUS;
  cfg.homeYaw = HOME_YAW; cfg.homePitch = HOME_PITCH; cfg.homeRoll = HOME_ROLL;
  cfg.wander = GAZE_WANDER;
  cfg.drift = GAZE_DRIFT;
  cfg.gazeSpeed = GAZE_SPEED;
  cfg.followYaw = FOLLOW_YAW;
  cfg.followPitch = FOLLOW_PITCH;
  cfg.wipeMs = WIPE_MS;
  cfg.eye.spacing = EYE_SPACING;
  cfg.eye.height = EYE_HEIGHT;
  cfg.eye.radius = EYE_WIDTH * 0.5f;
  cfg.eye.halfLen = fmaxf(EYE_LENGTH * 0.5f - cfg.eye.radius, 0.f);
  cfg.eye.tilt = EYE_TILT * eyes::DEG;
  cfg.screenSize = LCD_SIZE;
  anim.begin(cfg, esp_random(), millis());
  xform = eyes::makeXform(SCREEN_ROTATION, LCD_SIZE);
  dirty.begin(SCREEN_ROTATION, LCD_SIZE);

  // This board's ID: last 2 bytes of its factory MAC address, e.g. "3F2A" -> Bluetooth "RoboEyes-3F2A".
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  snprintf(boardId, sizeof boardId, "%02X%02X", mac[4], mac[5]);
  char name[24];
  snprintf(name, sizeof name, "%s-%s", BLE_NAME, boardId);
  ble_begin(name);
  emitHello();
  emitColors();
  emitMode();
}

void loop() {
  const uint32_t FRAME_US = 1000000UL / TARGET_FPS;
  static uint32_t nextFrameUs = micros();
  uint32_t startUs = micros();

  uint32_t now = millis();

  handleTouch(now);

  // the website (if connected) picks the mood, the gaze and when to blink
  pollLinks(now);
  bool isLinked = linked(now);
  anim.setLinked(isLinked);
  anim.setAutoBlink(!isLinked);
  anim.setMood(isLinked ? linkMood : eyes::NEUTRAL);
  if (isLinked) anim.setLook(linkX, linkY);

  anim.update(now);
  if (anim.takeRepaint()) dirty.invalidate();   // new colors from the web app: redraw everything
  saveColorsWhenSettled(now);
  const eyes::Scene &s = anim.scene();
  eyes::Prepared p;
  eyes::prepare(s, p);

  float r0, r1;
  bool ring = anim.wipeRing(r0, r1);
  eyes::Rect rects[eyes::DirtyTracker::MAX_RECTS];
  int n = dirty.collect(p, ring, r0, r1, rects);
  for (int i = 0; i < n; i++) pushRect(s, p, rects[i]);

#if SHOW_FPS
  static uint32_t frames = 0, busyUs = 0, reportMs = millis();
  frames++;
  busyUs += micros() - startUs;
  if (millis() - reportMs >= 10000) {
    float secs = (millis() - reportMs) / 1000.f;
    Serial.printf("D fps %.1f draw_ms %.1f\n", frames / secs, busyUs / 1000.f / frames);
    frames = 0; busyUs = 0; reportMs = millis();
  }
#endif

  // pace frames evenly (uneven frame timing reads as stutter)
  nextFrameUs += FRAME_US;
  int32_t wait = (int32_t)(nextFrameUs - micros());
  if (wait > 0) {
    if (wait > 1500) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
    while ((int32_t)(nextFrameUs - micros()) > 0) { }
  } else if (wait < -(int32_t)FRAME_US) {
    nextFrameUs = micros();   // fell behind: don't try to catch up in a burst
  }
}
