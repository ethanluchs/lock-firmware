#include "ble_service.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_gap_ble_api.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#include "app_config.h"
#include "control.h"
#include "diagnostics.h"

namespace ble {
namespace {

namespace proto = lock::proto;

BLEServer *g_server = nullptr;
BLECharacteristic *g_statusChar = nullptr;
BLE2902 *g_statusCccd = nullptr;

// The accepted (active) client. One at a time: a second client that connects
// while one is active is refused without touching any of this. Written by the
// BLE task, read by the control task (publishStatus, disconnect).
portMUX_TYPE g_connMux = portMUX_INITIALIZER_UNLOCKED;
bool g_haveConn = false;
uint16_t g_connId = 0;
esp_bd_addr_t g_connBda = {};
// The active client's status CCCD value. The library keeps one CCCD value for
// all links, so a refused client's CCCD write is undone from this copy, and
// notifications go only to the active client, based on this copy.
uint8_t g_cccd[2] = {0, 0};
// GATT interface for direct notifies (BLEServer::getGattsIf() is private).
esp_gatt_if_t g_gattsIf = ESP_GATT_IF_NONE;

// Log retrieval state. BLE task only (all GATT callbacks run there).
uint32_t g_logCursor = 0;
lock::log::LogEntry g_logEntries[proto::kLogPageMaxEntries];
uint8_t g_logPage[proto::kLogPageMaxLen];

class ServerCallbacks : public BLEServerCallbacks {
 public:
  using BLEServerCallbacks::onConnect;
  using BLEServerCallbacks::onDisconnect;

  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    uint16_t id = param->connect.conn_id;
    bool accepted;
    bool sameBda;
    portENTER_CRITICAL(&g_connMux);
    accepted = !g_haveConn;
    sameBda = memcmp(g_connBda, param->connect.remote_bda, sizeof(g_connBda)) == 0;
    if (accepted) {
      g_haveConn = true;
      g_connId = id;
      memcpy(g_connBda, param->connect.remote_bda, sizeof(g_connBda));
      g_cccd[0] = g_cccd[1] = 0;
    }
    portEXIT_CRITICAL(&g_connMux);

    if (accepted) {
      g_logCursor = 0;  // per-connection cursor (lock_protocol.h)
      // Unbonded clients don't keep subscriptions across connections; a new
      // client starts unsubscribed. RAM only, no flash.
      if (g_statusCccd) g_statusCccd->setNotifications(false);
      control::postConnect(id, true);
      return;
    }

    // Refuse the newcomer; the active client's state is left alone.
    control::postConnect(id, false);
    // A GAP disconnect by address would also drop the active link if it is
    // the same peer, so fall back to closing just this GATT connection then.
    if (sameBda || esp_ble_gap_disconnect(param->connect.remote_bda) != ESP_OK) {
      if (g_server) g_server->disconnect(id);
    }
  }

  void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    uint16_t id = param->disconnect.conn_id;
    bool wasActive;
    bool noActive;
    portENTER_CRITICAL(&g_connMux);
    wasActive = g_haveConn && g_connId == id;
    if (wasActive) {
      g_haveConn = false;
      g_cccd[0] = g_cccd[1] = 0;
    }
    noActive = !g_haveConn;
    portEXIT_CRITICAL(&g_connMux);
    // BLE-006: advertise again once no active client is connected. Never
    // while the active client is still connected (a refused newcomer's
    // disconnect must not restart it), hence advertiseOnDisconnect(false).
    if (noActive) BLEDevice::startAdvertising();
    control::postDisconnect(id, static_cast<uint16_t>(param->disconnect.reason), wasActive);
  }
};

bool isActive(uint16_t connId) {
  portENTER_CRITICAL(&g_connMux);
  bool active = g_haveConn && g_connId == connId;
  portEXIT_CRITICAL(&g_connMux);
  return active;
}

// Runs in the BLE task after the library has handled the event (so after
// BLEDescriptor stored a CCCD write). Tracks the active client's CCCD and
// undoes any other client's write to it.
void gattsHandler(esp_gatts_cb_event_t event, esp_gatt_if_t gattsIf,
                  esp_ble_gatts_cb_param_t *param) {
  if (event == ESP_GATTS_CONNECT_EVT) {
    portENTER_CRITICAL(&g_connMux);
    g_gattsIf = gattsIf;
    portEXIT_CRITICAL(&g_connMux);
    return;
  }
  if (event != ESP_GATTS_WRITE_EVT || !g_statusCccd || param->write.is_prep ||
      param->write.handle != g_statusCccd->getHandle()) {
    return;
  }
  uint8_t keep[2];
  bool active;
  portENTER_CRITICAL(&g_connMux);
  active = g_haveConn && g_connId == param->write.conn_id;
  if (active) {
    g_cccd[0] = param->write.len > 0 ? param->write.value[0] : 0;
    g_cccd[1] = param->write.len > 1 ? param->write.value[1] : 0;
  }
  memcpy(keep, g_cccd, sizeof(keep));
  portEXIT_CRITICAL(&g_connMux);
  if (!active) g_statusCccd->setValue(keep, sizeof(keep));
}

// CMD-003: capture and enqueue only. No actuation, flash or blocking here.
class CommandCallbacks : public BLECharacteristicCallbacks {
 public:
  using BLECharacteristicCallbacks::onWrite;

  void onWrite(BLECharacteristic *c, esp_ble_gatts_cb_param_t *param) override {
    // Only the active client may command the lock; a refused client being
    // dropped must not actuate it or reset the active client's idle timer.
    // (conn_id is the first field of both the write and exec-write params.)
    if (!param || !isActive(param->write.conn_id)) return;
    // getData/getLength also cover reassembled long (prepared) writes.
    control::postCommand(c->getData(), c->getLength());
  }
};

class LogCallbacks : public BLECharacteristicCallbacks {
 public:
  using BLECharacteristicCallbacks::onRead;
  using BLECharacteristicCallbacks::onWrite;

  void onWrite(BLECharacteristic *c, esp_ble_gatts_cb_param_t *param) override {
    // The cursor belongs to the active client only.
    if (!param || !isActive(param->write.conn_id)) return;
    // Wrong-length writes are ignored and leave the cursor unchanged.
    proto::decodeLogCursor(c->getData(), c->getLength(), &g_logCursor);
  }

  // Called for the first ATT read only; follow-up blob reads of a long page
  // are served by the library from the value set here. Other clients' reads
  // leave the value alone so they can't disturb the active client's page.
  void onRead(BLECharacteristic *c, esp_ble_gatts_cb_param_t *param) override {
    if (!param || !isActive(param->read.conn_id)) return;
    size_t n = control::readLog(g_logCursor, g_logEntries, proto::kLogPageMaxEntries);
    size_t len = proto::encodeLogPage(g_logEntries, n, g_logPage, sizeof(g_logPage));
    c->setValue(g_logPage, len);
  }
};

ServerCallbacks g_serverCb;
CommandCallbacks g_cmdCb;
LogCallbacks g_logCb;

}  // namespace

bool init() {
  if (!BLEDevice::init(cfg::kDeviceName)) {
    diag::logf(diag::kBle, "BLEDevice::init failed");
    return false;
  }
  // The library persists CCCD writes to NVS per peer address, even unbonded
  // ones; phones rotate addresses, so clear them each boot to bound NVS use.
  BLE2902::deleteAllPersistedValues();
  BLEDevice::setMTU(cfg::kLocalMtu);

  g_server = BLEDevice::createServer();
  if (!g_server) {
    diag::logf(diag::kBle, "createServer failed");
    return false;
  }
  g_server->setCallbacks(&g_serverCb);
  // BLE-006 is handled in onDisconnect: the library would restart advertising
  // on any disconnect, including a refused second client's.
  g_server->advertiseOnDisconnect(false);
  BLEDevice::setCustomGattsHandler(&gattsHandler);

  // 1 service + 4 x (decl + value) + 1 CCCD = 10 handles.
  BLEService *svc = g_server->createService(BLEUUID(proto::kServiceUuid), 16);
  if (!svc) {
    diag::logf(diag::kBle, "createService failed");
    return false;
  }

  BLECharacteristic *cmd =
      svc->createCharacteristic(proto::kCommandCharUuid, BLECharacteristic::PROPERTY_WRITE);
  cmd->setCallbacks(&g_cmdCb);

  g_statusChar = svc->createCharacteristic(
      proto::kStatusCharUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_statusCccd = new BLE2902();  // setup-time allocation, lives forever
  g_statusChar->addDescriptor(g_statusCccd);
  uint8_t status[proto::kStatusLen];
  control::currentStatus(status);
  g_statusChar->setValue(status, sizeof(status));

  BLECharacteristic *info =
      svc->createCharacteristic(proto::kInfoCharUuid, BLECharacteristic::PROPERTY_READ);
  uint8_t infoBytes[proto::kInfoLen];
  proto::encodeInfo(cfg::kFwMajor, cfg::kFwMinor, cfg::kFwPatch, infoBytes);
  info->setValue(infoBytes, sizeof(infoBytes));

  BLECharacteristic *logc = svc->createCharacteristic(
      proto::kLogCharUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  logc->setCallbacks(&g_logCb);

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(proto::kServiceUuid);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);  // iOS connection-interval workaround
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  diag::logf(diag::kBle, "advertising as %s (protocol %u.%u)", cfg::kDeviceName,
             proto::kProtocolMajor, proto::kProtocolMinor);
  return true;
}

void publishStatus(const uint8_t status[lock::proto::kStatusLen], bool notify) {
  if (!g_statusChar) return;
  g_statusChar->setValue(status, lock::proto::kStatusLen);
  if (!notify || !g_server) return;

  // Notify the active client only, and only if it subscribed. (The library's
  // notify() would also send to a refused client still being dropped.)
  portENTER_CRITICAL(&g_connMux);
  bool send = g_haveConn && (g_cccd[0] & 0x01);
  uint16_t id = g_connId;
  esp_gatt_if_t gattsIf = g_gattsIf;
  portEXIT_CRITICAL(&g_connMux);
  if (!send || gattsIf == ESP_GATT_IF_NONE) return;
  uint8_t buf[lock::proto::kStatusLen];
  memcpy(buf, status, sizeof(buf));
  esp_err_t err = esp_ble_gatts_send_indicate(gattsIf, id, g_statusChar->getHandle(),
                                              sizeof(buf), buf, false);
  if (err != ESP_OK) diag::logf(diag::kBle, "notify failed (err %d)", static_cast<int>(err));
}

void disconnect(uint16_t connId) {
  esp_bd_addr_t bda;
  bool match;
  portENTER_CRITICAL(&g_connMux);
  match = g_haveConn && g_connId == connId;
  if (match) memcpy(bda, g_connBda, sizeof(bda));
  portEXIT_CRITICAL(&g_connMux);

  // A GAP disconnect drops the physical link; gatts_close alone may not.
  esp_err_t err = match ? esp_ble_gap_disconnect(bda) : ESP_FAIL;
  if (err != ESP_OK && g_server) g_server->disconnect(connId);
}

}  // namespace ble
