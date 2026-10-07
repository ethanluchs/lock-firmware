#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#define SERVICE_UUID  "8f2a1c00-5b7e-4d19-9a3f-6c2e81d4b70a"
#define CMD_CHAR_UUID "8f2a1c01-5b7e-4d19-9a3f-6c2e81d4b70a"

const int ACTUATOR_PIN = 2;   // onboard LED stands in for the solenoid

// ---- actuator abstraction -------------------------------------------
// Everything that touches physical hardware goes behind these two calls.
// When the solenoid arrives, only this block changes.

void actuator_init() {
  pinMode(ACTUATOR_PIN, OUTPUT);
  digitalWrite(ACTUATOR_PIN, LOW);
}

void actuator_set(bool unlocked) {
  digitalWrite(ACTUATOR_PIN, unlocked ? HIGH : LOW);
}
// ---------------------------------------------------------------------

volatile bool g_pendingCmd = false;
volatile uint8_t g_cmdValue = 0;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    Serial.println("[ble] client connected");
  }
  void onDisconnect(BLEServer *pServer) {
    Serial.println("[ble] client disconnected, re-advertising");
    BLEDevice::startAdvertising();
  }
};

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    uint8_t *data = pChar->getData();
    size_t len = pChar->getLength();
    if (len < 1) return;

    Serial.printf("[ble] write: 0x%02X (%u bytes)\n", data[0], (unsigned)len);
    g_cmdValue = data[0];
    g_pendingCmd = true;          // hand off to loop(), do no work here
  }
};

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[boot] starting");

  actuator_init();

  if (!BLEDevice::init("VT-LOCK-001")) {
    Serial.println("[ble] init failed");
    return;
  }

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pCmd = pService->createCharacteristic(
      CMD_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE);
  pCmd->setCallbacks(new CmdCallbacks());

  pService->start();

  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);   // iOS connection-interval workaround
  pAdv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[ble] advertising as VT-LOCK-001");
}

void loop() {
  if (g_pendingCmd) {
    g_pendingCmd = false;
    uint8_t cmd = g_cmdValue;

    Serial.printf("[cmd] handling 0x%02X\n", cmd);
    if (cmd == 0x01) {
      actuator_set(true);
      Serial.println("[act] UNLOCKED");
    } else if (cmd == 0x00) {
      actuator_set(false);
      Serial.println("[act] LOCKED");
    } else {
      Serial.println("[cmd] unknown, ignoring");
    }
  }
  delay(10);
}