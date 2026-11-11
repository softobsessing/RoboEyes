/*
 * RoboEyes - animated eyes for the Waveshare ESP32-S3-Touch-AMOLED-1.43
 * (466x466 round AMOLED, SH8601 or CO5300 panel, FT3168 touch).
 *
 * - Idle: blue eyes that blink and glance around.
 * - Tap the screen: eyes morph into the "touched" expression and fade to orange.
 * - Tap again (or wait AUTO_RETURN_MS): back to idle.
 *
 * Arduino IDE settings (Tools menu):
 *   Board:            ESP32S3 Dev Module
 *   USB CDC On Boot:  Enabled
 *   Flash Size:       16MB
 *   PSRAM:            OPI PSRAM
 * Board package: "esp32 by Espressif Systems" 3.1.x or newer. No extra libraries needed.
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "esp_lcd_sh8601.h"
#include "read_lcd_id_bsp.h"
#include "eye_types.h"

// =====================================================================
//  LOOK & FEEL - tweak these
// =====================================================================

// Colors as 0xRRGGBB
static const uint32_t IDLE_COLOR    = 0x19C8FF;   // blue
static const uint32_t TOUCHED_COLOR = 0xFF7A00;   // orange

// 0 = tap toggles between moods; otherwise return to idle after this many ms
static const uint32_t AUTO_RETURN_MS = 0;

// How fast the moods morph into each other (0..1 per frame, higher = faster)
static const float MORPH_SPEED = 0.12f;

// One eye shape. All sizes are in pixels on the 466x466 screen.
struct EyeShape {
  float w, h;          // eye width / height
  float radius;        // corner roundness
  float spacing;       // distance between the two eye centers
  float yOffset;       // vertical shift from screen center (+ = down)
  float topLid;        // 0..1  how far the upper lid comes down
  float lidSlant;      // pixels the lid drops toward the nose (+ = angry, - = sad)
  float bottomLid;     // 0..1  "happy" curved lower lid pushing up
};

// Idle expression (Image 1)
static const EyeShape IDLE_SHAPE = {
  /*w*/ 110, /*h*/ 170, /*radius*/ 40, /*spacing*/ 190, /*yOffset*/ 0,
  /*topLid*/ 0.0f, /*lidSlant*/ 0, /*bottomLid*/ 0.0f
};

// Touched expression (Image 2)
static const EyeShape TOUCHED_SHAPE = {
  /*w*/ 130, /*h*/ 150, /*radius*/ 45, /*spacing*/ 200, /*yOffset*/ 0,
  /*topLid*/ 0.0f, /*lidSlant*/ 0, /*bottomLid*/ 0.55f
};

// Rotate the picture if it's sideways on your board: 0, 90, 180, 270
static const int SCREEN_ROTATION = 0;

// Screen brightness 0..255
static const uint8_t BRIGHTNESS = 255;

// =====================================================================
//  BOARD PINS (Waveshare ESP32-S3-Touch-AMOLED-1.43) - don't change
// =====================================================================
#define LCD_W        466
#define LCD_H        466
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
#define STRIP_ROWS   32     // rows sent per DMA transfer (must be even)

// =====================================================================
//  Display driver (uses Waveshare's esp_lcd_sh8601 driver)
// =====================================================================
static esp_lcd_panel_handle_t panel = nullptr;
static esp_lcd_panel_io_handle_t panelIo = nullptr;
static int xGap = 0;                       // CO5300 panels need a 6px column offset
static uint16_t *dmaBuf[2];
static SemaphoreHandle_t bufFree;          // counts DMA buffers that are free to draw into

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
  // 0x51 = write display brightness; QSPI command format is 0x02 00 CMD 00
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
  bus.max_transfer_sz = LCD_W * STRIP_ROWS * 2 + 64;
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
    dmaBuf[i] = (uint16_t *)heap_caps_malloc(LCD_W * STRIP_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(dmaBuf[i]);
  }
  bufFree = xSemaphoreCreateCounting(2, 2);
}

// =====================================================================
//  Touch (FT3168)
// =====================================================================
static void touchInit() {
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0x00);   // device mode register
  Wire.write(0x00);   // normal operating mode
  Wire.endTransmission();
}

static bool isTouched() {
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0x02);   // number of touch points
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(FT3168_ADDR, 1) != 1) return false;
  uint8_t n = Wire.read() & 0x0F;
  return n > 0 && n < 6;
}

// =====================================================================
//  Eye renderer
// =====================================================================


static EyeDraw eyes[2];
static uint8_t colR, colG, colB;

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Coverage (0..1) of one eye at pixel center (px,py) - anti-aliased SDF
static float eyeCoverage(const EyeDraw &e, float px, float py, float side) {
  // rounded rectangle
  float qx = fabsf(px - e.cx) - (e.hw - e.r);
  float qy = fabsf(py - e.cy) - (e.hh - e.r);
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  float inside = fminf(fmaxf(qx, qy), 0.0f);
  float d = sqrtf(ox * ox + oy * oy) + inside - e.r;
  float cov = clamp01(0.5f - d);
  if (cov <= 0) return 0;

  // upper eyelid: everything above a (possibly slanted) line is hidden
  float lineY = e.lidY + e.lidSlant * side * (px - e.cx) / e.hw;
  cov *= clamp01(py - lineY + 0.5f);

  // lower "happy" lid: hide pixels inside a circle below the eye
  if (e.hasSmile && cov > 0) {
    float dx = px - e.smileCx, dy = py - e.smileCy;
    float ds = sqrtf(dx * dx + dy * dy) - e.smileR;
    cov *= clamp01(ds + 0.5f);
  }
  return cov;
}

static inline uint16_t rgb565swapped(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (c >> 8) | (c << 8);   // panel wants big-endian
}

// Map a framebuffer pixel back to "logical" coords so SCREEN_ROTATION works
static inline void rotatePoint(float x, float y, float &ox, float &oy) {
  switch (SCREEN_ROTATION) {
    case 90:  ox = y;             oy = LCD_W - 1 - x; break;
    case 180: ox = LCD_W - 1 - x; oy = LCD_H - 1 - y; break;
    case 270: ox = LCD_H - 1 - y; oy = x;             break;
    default:  ox = x;             oy = y;             break;
  }
}

static void renderStrip(uint16_t *buf, int yStart, int rows) {
  memset(buf, 0, LCD_W * rows * 2);
  for (int i = 0; i < 2; i++) {
    const EyeDraw &e = eyes[i];
    float side = (i == 0) ? 1.0f : -1.0f;   // mirror the lid slant for each eye
    // bounding box in physical coords
    int bx0, by0, bx1, by1;
    if (SCREEN_ROTATION == 0) { bx0 = e.x0; by0 = e.y0; bx1 = e.x1; by1 = e.y1; }
    else { bx0 = 0; by0 = 0; bx1 = LCD_W - 1; by1 = LCD_H - 1; }
    int ya = max(by0, yStart), yb = min(by1, yStart + rows - 1);
    int xa = max(bx0, 0), xb = min(bx1, LCD_W - 1);
    for (int y = ya; y <= yb; y++) {
      uint16_t *row = buf + (y - yStart) * LCD_W;
      for (int x = xa; x <= xb; x++) {
        float lx, ly;
        rotatePoint(x + 0.5f, y + 0.5f, lx, ly);
        if (SCREEN_ROTATION != 0 && (lx < e.x0 || lx > e.x1 + 1 || ly < e.y0 || ly > e.y1 + 1)) continue;
        float c = eyeCoverage(e, lx, ly, side);
        if (c <= 0) continue;
        row[x] = rgb565swapped(colR * c, colG * c, colB * c);
      }
    }
  }
}

// Push rows [y0, y1] (inclusive) of the current scene to the panel
static void pushRows(int y0, int y1) {
  y0 &= ~1;                      // panel needs even start row ...
  y1 |= 1;                       // ... and odd end row
  if (y0 < 0) y0 = 0;
  if (y1 > LCD_H - 1) y1 = LCD_H - 1;
  static int which = 0;
  for (int y = y0; y <= y1; y += STRIP_ROWS) {
    int rows = min(STRIP_ROWS, y1 - y + 1);
    xSemaphoreTake(bufFree, portMAX_DELAY);
    renderStrip(dmaBuf[which], y, rows);
    esp_lcd_panel_draw_bitmap(panel, xGap, y, xGap + LCD_W, y + rows, dmaBuf[which]);
    which ^= 1;
  }
}

// =====================================================================
//  Animation state
// =====================================================================
static float mood = 0;            // 0 = idle, 1 = touched (animated)
static float moodTarget = 0;
static float blink = 1;           // 1 = open, 0 = closed
static uint32_t nextBlink = 0;
static int blinkPhase = 0;        // 0 none, 1 closing, 2 opening
static float lookX = 0, lookY = 0, lookTX = 0, lookTY = 0;
static uint32_t nextLook = 0;
static uint32_t touchedAt = 0;
static bool wasTouched = false;

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static void buildEyes() {
  float t = mood * mood * (3 - 2 * mood);   // smoothstep
  EyeShape s;
  s.w         = lerpf(IDLE_SHAPE.w,         TOUCHED_SHAPE.w,         t);
  s.h         = lerpf(IDLE_SHAPE.h,         TOUCHED_SHAPE.h,         t);
  s.radius    = lerpf(IDLE_SHAPE.radius,    TOUCHED_SHAPE.radius,    t);
  s.spacing   = lerpf(IDLE_SHAPE.spacing,   TOUCHED_SHAPE.spacing,   t);
  s.yOffset   = lerpf(IDLE_SHAPE.yOffset,   TOUCHED_SHAPE.yOffset,   t);
  s.topLid    = lerpf(IDLE_SHAPE.topLid,    TOUCHED_SHAPE.topLid,    t);
  s.lidSlant  = lerpf(IDLE_SHAPE.lidSlant,  TOUCHED_SHAPE.lidSlant,  t);
  s.bottomLid = lerpf(IDLE_SHAPE.bottomLid, TOUCHED_SHAPE.bottomLid, t);

  colR = lerpf((IDLE_COLOR >> 16) & 0xFF, (TOUCHED_COLOR >> 16) & 0xFF, t);
  colG = lerpf((IDLE_COLOR >> 8) & 0xFF,  (TOUCHED_COLOR >> 8) & 0xFF,  t);
  colB = lerpf(IDLE_COLOR & 0xFF,         TOUCHED_COLOR & 0xFF,         t);

  float h = fmaxf(s.h * blink, 6.0f);
  float r = fminf(s.radius, fminf(s.w, h) * 0.5f);

  for (int i = 0; i < 2; i++) {
    EyeDraw &e = eyes[i];
    float dir = (i == 0) ? -1.0f : 1.0f;   // eye 0 = left, eye 1 = right
    e.cx = LCD_W / 2.0f + dir * s.spacing / 2 + lookX;
    e.cy = LCD_H / 2.0f + s.yOffset + lookY;
    e.hw = s.w / 2;
    e.hh = h / 2;
    e.r = r;
    // upper lid line
    e.lidY = (e.cy - e.hh) + s.topLid * h;
    e.lidSlant = s.lidSlant;   // mirrored per eye in eyeCoverage()
    // happy lower lid: big circle rising from below
    e.hasSmile = s.bottomLid > 0.01f;
    e.smileR = s.w * 0.9f;
    e.smileCx = e.cx;
    e.smileCy = (e.cy + e.hh) + e.smileR - s.bottomLid * h;

    e.x0 = (int)floorf(e.cx - e.hw) - 1;
    e.x1 = (int)ceilf(e.cx + e.hw) + 1;
    e.y0 = (int)floorf(e.cy - e.hh) - 1;
    e.y1 = (int)ceilf(e.cy + e.hh) + 1;
  }
}

static void updateAnimation(uint32_t now) {
  // --- touch ---
  bool touched = isTouched();
  if (touched && !wasTouched) {
    moodTarget = (moodTarget > 0.5f) ? 0.0f : 1.0f;
    touchedAt = now;
    blinkPhase = 0; blink = 1;
  }
  wasTouched = touched;
  if (AUTO_RETURN_MS && moodTarget > 0.5f && now - touchedAt > AUTO_RETURN_MS) moodTarget = 0;

  mood += (moodTarget - mood) * MORPH_SPEED;
  if (fabsf(moodTarget - mood) < 0.002f) mood = moodTarget;

  // --- blinking ---
  if (blinkPhase == 0 && now > nextBlink) blinkPhase = 1;
  if (blinkPhase == 1) { blink -= 0.25f; if (blink <= 0) { blink = 0; blinkPhase = 2; } }
  else if (blinkPhase == 2) {
    blink += 0.18f;
    if (blink >= 1) { blink = 1; blinkPhase = 0; nextBlink = now + random(2000, 5000); }
  }

  // --- glancing around ---
  if (now > nextLook) {
    if (random(3) == 0) { lookTX = 0; lookTY = 0; }
    else { lookTX = random(-45, 46); lookTY = random(-30, 31); }
    nextLook = now + random(1200, 3500);
  }
  lookX += (lookTX - lookX) * 0.15f;
  lookY += (lookTY - lookY) * 0.15f;
}

// =====================================================================
//  Arduino entry points
// =====================================================================
static int prevY0 = 0, prevY1 = LCD_H - 1;   // first frame clears the whole screen

void setup() {
  Serial.begin(115200);
  delay(200);
  displayInit();
  touchInit();
  setBrightness(BRIGHTNESS);
  randomSeed(esp_random());
  nextBlink = millis() + 1500;
  Serial.println("RoboEyes ready - tap the screen!");
}

void loop() {
  uint32_t now = millis();
  updateAnimation(now);
  buildEyes();

  // only redraw the band of rows the eyes touch (this frame + last frame)
  int y0 = min(eyes[0].y0, eyes[1].y0);
  int y1 = max(eyes[0].y1, eyes[1].y1);
  if (SCREEN_ROTATION != 0) { y0 = 0; y1 = LCD_H - 1; }
  pushRows(min(y0, prevY0) - 2, max(y1, prevY1) + 2);
  prevY0 = y0; prevY1 = y1;

  // ~50 fps cap
  static uint32_t last = 0;
  uint32_t el = millis() - last;
  if (el < 20) delay(20 - el);
  last = millis();
}
