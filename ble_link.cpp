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

static BLECharacteristic* txChar = nullptr;
static StreamBufferHandle_t rxStream = nullptr;   // BLE task -> loop()
static volatile bool connected = false;

class ServerEvents : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { connected = true; }
  void onDisconnect(BLEServer* s) override {
    connected = false;
    s->startAdvertising();   // let the web app reconnect
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
  BLEServer* server = BLEDevice::createServer();
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
  if (!connected || !txChar) return;
  String s(line);
  s += '\n';
  txChar->setValue((uint8_t*)s.c_str(), s.length());
  txChar->notify();
}

bool ble_connected() { return connected; }
