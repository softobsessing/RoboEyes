/*
 * RoboEyes - two pill-shaped eyes for the Waveshare ESP32-S3-Touch-AMOLED-1.43
 * (466x466 round AMOLED, FT3168 touch).
 *
 * The round screen itself is the character's face: it's rendered as a 3D ball
 * exactly the size of the display, so only the eyes are drawn on it.
 *
 * - Idle: white face, black eyes. Both eyes blink together and glide around.
 * - Tap: the eyes look around and the face inverts with a circular wipe
 *   (black face, white eyes).
 * - Tap again: back to white. (Or set AUTO_RETURN_MS to revert by itself.)
 *
 * Website control: https://softobsessing.github.io/doomscroll-eyes/ connects
 * over USB or Bluetooth ("EyeBuddy") and picks the mood (neutral / happy /
 * side-eye / disappointed) and where the eyes look. Its live preview runs the
 * same eye code, so the website and the board show the same thing.
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

// Colors as 0xRRGGBB
#define IDLE_FACE         0xFFFFFF   // before tapping: white screen...
#define IDLE_EYES         0x000000   // ...black eyes
#define TAP_FACE          0x0B0B0E   // after tapping: black screen...
#define TAP_EYES          0xF4F4F4   // ...white eyes

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
#define WIPE_MS           650        // speed of the white/black inversion
#define AUTO_RETURN_MS    0          // 0 = tap toggles; else go back to white after N ms

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

// Website link. Keep BLE_NAME = "EyeBuddy": that's the name the website looks for.
#define BLE_NAME          "EyeBuddy"
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

static bool isTouched() {
  Wire.beginTransmission(FT3168_ADDR);
  Wire.write(0x02);   // number of touch points
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(FT3168_ADDR, 1) != 1) return false;
  uint8_t n = Wire.read() & 0x0F;
  return n > 0 && n < 6;
}

static void touchTask(void *) {
  bool down = false;
  uint32_t lastDown = 0;
  for (;;) {
    bool now = isTouched();
    uint32_t ms = millis();
    if (now && !down && ms - lastDown > 250) { down = true; lastDown = ms; touchDownPending = true; }
    else if (!now && down) { down = false; touchUpPending = true; }
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
//  Website link (same text protocol over USB serial and Bluetooth)
//   website -> board   E <neutral|happy|side|sad> <lookX -100..100> <lookY -100..100>   (~20/s)
//                      B | B2          blink now / double blink (the website is the blink clock)
//                      P               ping
//   board -> website   HELLO RoboEyes 1
//                      T touch | T release
//                      C light | C dark     colors (sent on HELLO and whenever a tap flips them)
//                      D fps <n> draw_ms <n>   (USB only, every 10 s; the website ignores it)
// =====================================================================
static bool haveLink = false;
static uint32_t lastLinkMs = 0;
static eyes::Mood linkMood = eyes::NEUTRAL;
static float linkX = 0, linkY = 0;

static void emit(const char *line) {
  Serial.println(line);
  ble_send_line(line);
}

static void emitColors() { emit(anim.isInverted() ? "C dark" : "C light"); }

static bool linked(uint32_t now) { return haveLink && now - lastLinkMs < LINK_TIMEOUT_MS; }

static void onLine(const char *line, uint32_t now) {
  if (line[0] == 'P' && line[1] == 0) {
    emit("HELLO RoboEyes 1");
    emitColors();
    return;
  }
  if (line[0] == 'B' && (line[1] == 0 || (line[1] == '2' && line[2] == 0))) {
    if (linked(now)) anim.blinkNow(now, line[1] == '2');
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
  cfg.idleFace = IDLE_FACE;  cfg.idleEyes = IDLE_EYES;
  cfg.tapFace = TAP_FACE;    cfg.tapEyes = TAP_EYES;
  cfg.faceRadius = FACE_RADIUS;
  cfg.homeYaw = HOME_YAW; cfg.homePitch = HOME_PITCH; cfg.homeRoll = HOME_ROLL;
  cfg.wander = GAZE_WANDER;
  cfg.drift = GAZE_DRIFT;
  cfg.gazeSpeed = GAZE_SPEED;
  cfg.followYaw = FOLLOW_YAW;
  cfg.followPitch = FOLLOW_PITCH;
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
  dirty.begin(SCREEN_ROTATION, LCD_SIZE);
  ble_begin(BLE_NAME);
  emit("HELLO RoboEyes 1");
  emitColors();
}

void loop() {
  const uint32_t FRAME_US = 1000000UL / TARGET_FPS;
  static uint32_t nextFrameUs = micros();
  uint32_t startUs = micros();

  uint32_t now = millis();

  // touch: a tap looks around and flips the colors; tell the website so its preview follows
  if (touchDownPending) {
    touchDownPending = false;
    anim.tap(now);
    emit("T touch");
    emitColors();
  }
  if (touchUpPending) { touchUpPending = false; emit("T release"); }

  // the website (if connected) picks the mood, the gaze and when to blink
  pollLinks(now);
  bool isLinked = linked(now);
  anim.setLinked(isLinked);
  anim.setAutoBlink(!isLinked);
  anim.setMood(isLinked ? linkMood : eyes::NEUTRAL);
  if (isLinked) anim.setLook(linkX, linkY);

  anim.update(now);
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
