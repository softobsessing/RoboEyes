#include "ble_link.h"
#include <Arduino.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLE2902.h>
#endif
#include <BLEDevice.h>
#include <BLEServer.h>
#include <freertos/stream_buffer.h>

#define NUS_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // laptop -> board
#define NUS_TX      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // board -> laptop
constexpr size_t CHUNK = 20;                                  // fits the smallest BLE packet (ATT MTU 23)

static BLEServer* server = nullptr;
static BLECharacteristic* txChar = nullptr;
static StreamBufferHandle_t rxStream = nullptr;   // BLE task -> loop()
static volatile int connections = 0;
static volatile uint32_t drops = 0;

class ServerEvents : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    connections++;
    // NimBLE stops advertising when someone connects, so a second laptop can't find the board.
    // Belt and braces: if one slips in anyway, drop all and let the rightful one reconnect.
    if (connections > 1) {
      for (auto& p : s->getPeerDevices(false)) s->disconnect(p.first);
    }
  }
  void onDisconnect(BLEServer* s) override {
    if (connections > 0) connections--;
    drops++;
    if (connections == 0) s->startAdvertising();   // let the web app reconnect
  }
};

class RxEvents : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    xStreamBufferSend(rxStream, c->getData(), c->getLength(), 0);
  }
};

void ble_begin(const char* name) {
  rxStream = xStreamBufferCreate(1024, 1);
  BLEDevice::init(name);
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerEvents());
  BLEService* nus = server->createService(NUS_SERVICE);
  txChar = nus->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  txChar->addDescriptor(new BLE2902());   // Bluedroid needs it for notifications; NimBLE adds it itself
#endif
  BLECharacteristic* rx = nus->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxEvents());
  nus->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);   // the name goes in the scan response; the 128-bit UUID fills the advert
  BLEDevice::startAdvertising();
}

size_t ble_read(char* buf, size_t max) { return rxStream ? xStreamBufferReceive(rxStream, buf, max, 0) : 0; }

void ble_send_line(const char* line) {
  if (connections <= 0 || !txChar) return;
  String s(line);
  s += '\n';
  for (size_t off = 0; off < s.length(); off += CHUNK) {   // the web app reassembles lines
    size_t n = s.length() - off < CHUNK ? s.length() - off : CHUNK;
    txChar->setValue((uint8_t*)s.c_str() + off, n);
    txChar->notify();
  }
}

bool ble_connected() { return connections > 0; }
uint32_t ble_drops() { return drops; }

void ble_disconnect() {
  if (!server) return;
  for (auto& p : server->getPeerDevices(false)) server->disconnect(p.first);
}
