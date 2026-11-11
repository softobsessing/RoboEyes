/*
 * RoboEyes - a ball character with two pill-shaped eyes for the
 * Waveshare ESP32-S3-Touch-AMOLED-1.43 (466x466 round AMOLED, FT3168 touch).
 *
 * - Idle: white ball on a black background. The eyes are cut-outs, blink
 *   together, and the ball drifts and glances around.
 * - Tap: the ball looks around and the screen inverts with a circular wipe
 *   (white background, black ball).
 * - Tap again: back to black. (Or set AUTO_RETURN_MS to revert by itself.)
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
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "esp_lcd_sh8601.h"
#include "read_lcd_id_bsp.h"
#include "eyes_render.h"
#include "eyes_anim.h"

// =====================================================================
//  LOOK & FEEL - tweak these
// =====================================================================

// Colors as 0xRRGGBB
#define IDLE_BACKGROUND   0x000000   // before tapping: black screen...
#define IDLE_BALL         0xFFFFFF   // ...white ball
#define TAP_BACKGROUND    0xF4F4F4   // after tapping: white screen...
#define TAP_BALL          0x0B0B0E   // ...black ball

#define BALL_RADIUS       165        // px (screen is 466 px wide)

// Resting gaze, matched to the reference images (looking up and to the right)
#define HOME_YAW          28         // degrees, + = right
#define HOME_PITCH        27         // degrees, + = up
#define HOME_ROLL         -10        // degrees, + = clockwise tilt

#define GAZE_WANDER       10         // degrees the gaze drifts around while idle
#define BALL_DRIFT        12         // px the ball floats around while idle
#define WIPE_MS           550        // speed of the black/white inversion
#define AUTO_RETURN_MS    0          // 0 = tap toggles; else go back to black after N ms

// Eye shape, in fractions of the ball radius
#define EYE_SPACING       0.25f      // half the distance between the eyes
#define EYE_HEIGHT        0.00f      // + = lower on the face
#define EYE_WIDTH         0.20f      // pill width
#define EYE_LENGTH        0.42f      // pill length (tip to tip)
#define EYE_TILT          0          // degrees, leans the pill tops outward

#define SCREEN_ROTATION   0          // 0, 90, 180, 270 if it looks sideways
#define BRIGHTNESS        255        // 0..255

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
#define STRIP_ROWS   24     // rows per DMA transfer (must be even)

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
  bus.max_transfer_sz = LCD_SIZE * STRIP_ROWS * 2 + 64;
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
    dmaBuf[i] = (uint16_t *)heap_caps_malloc(LCD_SIZE * STRIP_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
//  Drawing
// =====================================================================
static eyes::Animator anim;
static eyes::Xform xform;

// Physical bounding box of the ball (with a margin), honoring SCREEN_ROTATION.
static eyes::Rect ballRect(const eyes::Scene &s) {
  float W = LCD_SIZE, m = 3;
  float lx0 = s.ballX - s.ballR - m, lx1 = s.ballX + s.ballR + m;
  float ly0 = s.ballY - s.ballR - m, ly1 = s.ballY + s.ballR + m;
  float ax, ay, bx, by;   // logical -> physical for two opposite corners
  switch (SCREEN_ROTATION) {
    case 90:  ax = W - ly0; ay = lx0; bx = W - ly1; by = lx1; break;
    case 180: ax = W - lx0; ay = W - ly0; bx = W - lx1; by = W - ly1; break;
    case 270: ax = ly0; ay = W - lx0; bx = ly1; by = W - lx1; break;
    default:  ax = lx0; ay = ly0; bx = lx1; by = ly1; break;
  }
  return { (int)floorf(fminf(ax, bx)), (int)floorf(fminf(ay, by)),
           (int)ceilf(fmaxf(ax, bx)), (int)ceilf(fmaxf(ay, by)) };
}

// Render and send a rectangle in strips, double-buffered over DMA.
static void pushRect(const eyes::Scene &s, eyes::Rect r) {
  // panel needs even start and odd end coordinates
  r.x0 = max(r.x0, 0) & ~1;
  r.y0 = max(r.y0, 0) & ~1;
  r.x1 = min(r.x1 | 1, LCD_SIZE - 1);
  r.y1 = min(r.y1 | 1, LCD_SIZE - 1);
  int w = r.x1 - r.x0 + 1;

  eyes::Prepared p;
  eyes::prepare(s, p);
  static int which = 0;
  for (int y = r.y0; y <= r.y1; y += STRIP_ROWS) {
    int rows = min(STRIP_ROWS, r.y1 - y + 1);
    xSemaphoreTake(bufFree, portMAX_DELAY);
    eyes::renderRect(s, p, xform, dmaBuf[which], r.x0, y, w, rows);
    esp_lcd_panel_draw_bitmap(panel, r.x0 + xGap, y, r.x1 + 1 + xGap, y + rows, dmaBuf[which]);
    which ^= 1;
  }
}

// =====================================================================
//  Arduino entry points
// =====================================================================
static eyes::Rect prevRect;
static bool firstFrame = true;

void setup() {
  Serial.begin(115200);
  delay(200);
  displayInit();
  touchInit();
  setBrightness(BRIGHTNESS);

  eyes::AnimConfig cfg;
  cfg.idleBg = IDLE_BACKGROUND;  cfg.idleBall = IDLE_BALL;
  cfg.tapBg = TAP_BACKGROUND;    cfg.tapBall = TAP_BALL;
  cfg.ballRadius = BALL_RADIUS;
  cfg.homeYaw = HOME_YAW; cfg.homePitch = HOME_PITCH; cfg.homeRoll = HOME_ROLL;
  cfg.wander = GAZE_WANDER;
  cfg.drift = BALL_DRIFT;
  cfg.wipeMs = WIPE_MS;
  cfg.autoReturnMs = AUTO_RETURN_MS;
  cfg.eye.spacing = EYE_SPACING;
  cfg.eye.height = EYE_HEIGHT;
  cfg.eye.radius = EYE_WIDTH * 0.5f;
  cfg.eye.halfLen = fmaxf(EYE_LENGTH * 0.5f - cfg.eye.radius, 0.f);
  cfg.eye.tilt = EYE_TILT * eyes::DEG;
  cfg.screenSize = LCD_SIZE;
  anim.begin(cfg, esp_random(), millis());
  xform = eyes::makeXform(SCREEN_ROTATION, LCD_SIZE);
  Serial.println("RoboEyes ready - tap the screen!");
}

void loop() {
  static bool wasTouched = false;
  static uint32_t lastTap = 0;
  uint32_t now = millis();

  bool touched = isTouched();
  bool tap = touched && !wasTouched && (now - lastTap > 250);
  if (tap) lastTap = now;
  wasTouched = touched;

  anim.update(now, tap);
  const eyes::Scene &s = anim.scene();

  eyes::Rect r = ballRect(s);
  if (firstFrame || anim.needsFullRedraw()) {
    // background is changing: redraw everything
    pushRect(s, { 0, 0, LCD_SIZE - 1, LCD_SIZE - 1 });
    firstFrame = false;
  } else {
    // only the area the ball covered last frame and covers now
    eyes::Rect u = { min(r.x0, prevRect.x0), min(r.y0, prevRect.y0),
               max(r.x1, prevRect.x1), max(r.y1, prevRect.y1) };
    pushRect(s, u);
  }
  prevRect = r;

  // cap at ~60 fps
  static uint32_t last = 0;
  uint32_t el = millis() - last;
  if (el < 16) delay(16 - el);
  last = millis();
}
