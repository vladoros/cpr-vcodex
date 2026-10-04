#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_CLAUDE_BUDDY && !defined(SIMULATOR)

#include <Arduino.h>
#include <BuddyProtocol.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <esp_random.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "BuddyTransport.h"

namespace {
constexpr char kServiceUuid[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kRxUuid[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kTxUuid[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr size_t kRingBytes = 4096;
constexpr uint16_t kRequestedMtu = 517;
constexpr size_t kMaxNotifyChunk = 180;
constexpr uint16_t kDefaultMtu = 23;
constexpr uint32_t kDisconnectWaitMs = 2000;
constexpr uint32_t kTeardownSettleMs = 20;
}  // namespace

class BleNusPeripheral final : public BuddyTransport,
                               private NimBLEServerCallbacks,
                               private NimBLECharacteristicCallbacks {
 public:
  ~BleNusPeripheral() override { end(); }

  bool begin() override {
    if (started_) return true;
    powerManager.setPowerSaving(false);

    ring_ = makePsramByteBufferNoThrow(kRingBytes);
    if (!ring_) ring_ = makeHeapByteBufferNoThrow(kRingBytes);
    ringMutex_ = xSemaphoreCreateMutex();
    if (!ring_ || !ringMutex_) {
      LOG_ERR("BUDDY", "Cannot allocate BLE RX ring");
      releaseRing();
      return false;
    }
    head_ = tail_ = 0;

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(name_, sizeof(name_), "Claude-%02X%02X", mac[4], mac[5]);

    if (!NimBLEDevice::init(name_)) {
      LOG_ERR("BUDDY", "NimBLEDevice::init failed");
      releaseRing();
      return false;
    }
    NimBLEDevice::setMTU(kRequestedMtu);
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(this, false);
    NimBLEService* service = server_->createService(kServiceUuid);
    rx_ = service->createCharacteristic(
        kRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    rx_->setCallbacks(this);
    tx_ = service->createCharacteristic(kTxUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
    tx_->setCallbacks(this);
    service->start();

    stopping_ = false;
    rxWriteLogs_ = 0;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->enableScanResponse(true);
    const bool uuidOk = adv->addServiceUUID(kServiceUuid);
    const bool nameOk = adv->setName(name_);
    if (!uuidOk || !nameOk) {
      LOG_ERR("BUDDY", "Advertisement setup failed: uuid=%d name=%d", uuidOk, nameOk);
      NimBLEDevice::deinit(true);
      server_ = nullptr;
      releaseRing();
      return false;
    }
    if (!adv->start()) {
      LOG_ERR("BUDDY", "Advertising start failed");
      NimBLEDevice::deinit(true);
      releaseRing();
      return false;
    }
    started_ = true;
    LOG_INF("BUDDY", "Advertising as %s, bonds=%d", name_, NimBLEDevice::getNumBonds());
    return true;
  }

  void end() override {
    if (!started_) return;
    stopping_ = true;
    LOG_INF("BUDDY", "BLE stopping, link %s", connected_ ? "up" : "down");
    NimBLEDevice::getAdvertising()->stop();
    if (connected_ && server_) {
      server_->disconnect(connHandle_);
      const uint32_t startMs = millis();
      while (connected_ && millis() - startMs < kDisconnectWaitMs) delay(10);
      LOG_INF("BUDDY", "Link %s after %u ms", connected_ ? "still up" : "closed",
              static_cast<unsigned>(millis() - startMs));
    }
    delay(kTeardownSettleMs);
    started_ = false;
    connected_ = false;
    secure_ = false;
    passkey_ = 0;
    LOG_INF("BUDDY", "NimBLE deinit begin");
    NimBLEDevice::deinit(true);
    LOG_INF("BUDDY", "NimBLE deinit done");
    server_ = nullptr;
    rx_ = nullptr;
    tx_ = nullptr;
    releaseRing();
    LOG_INF("BUDDY", "BLE stopped");
  }

  size_t read(uint8_t* out, size_t cap) override {
    if (!started_ || !ringMutex_) return 0;
    size_t n = 0;
    if (xSemaphoreTake(ringMutex_, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    while (n < cap && tail_ != head_) {
      out[n++] = ring_[tail_];
      tail_ = (tail_ + 1) % kRingBytes;
    }
    xSemaphoreGive(ringMutex_);
    return n;
  }

  bool send(const char* line) override {
    if (!started_ || !connected_ || !tx_) return false;
    const uint16_t handle = connHandle_;
    const size_t chunk = std::min<size_t>(std::max<size_t>(mtu_, kDefaultMtu) - 3, kMaxNotifyChunk);
    const size_t len = strlen(line);
    uint8_t buf[kMaxNotifyChunk];
    for (size_t off = 0;;) {
      const size_t n = claude_buddy::frameChunk(line, len, off, chunk, buf);
      if (n == 0) break;
      if (!tx_->notify(buf, n, handle)) {
        LOG_ERR("BUDDY", "Notify failed at offset %u", static_cast<unsigned>(off));
        return false;
      }
      off += n;
      if (off < len + 1) delay(4);
    }
    return true;
  }

  bool connected() const override { return connected_; }
  bool secure() const override { return secure_; }
  uint32_t passkey() const override { return passkey_; }
  void deleteBonds() override {
    if (!started_) return;
    const int before = NimBLEDevice::getNumBonds();
    const bool ok = NimBLEDevice::deleteAllBonds();
    LOG_INF("BUDDY", "Delete bonds: ok=%d, bonds %d -> %d", ok, before, NimBLEDevice::getNumBonds());
  }
  const char* advertisedName() const override { return name_; }

 private:
  void releaseRing() {
    if (ringMutex_) {
      vSemaphoreDelete(ringMutex_);
      ringMutex_ = nullptr;
    }
    ring_.reset();
  }

  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    LOG_INF("BUDDY", "Central %s connected: bonded=%d encrypted=%d mtu=%u, host stack free %u B",
            info.getAddress().toString().c_str(), info.isBonded(), info.isEncrypted(), info.getMTU(),
            uxTaskGetStackHighWaterMark(nullptr));
    connHandle_ = info.getConnHandle();
    mtu_ = kDefaultMtu;
    secure_ = info.isEncrypted();
    connected_ = true;
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    LOG_INF("BUDDY", "Central disconnected, reason %d", reason);
    connected_ = false;
    secure_ = false;
    passkey_ = 0;
    mtu_ = kDefaultMtu;
    if (!stopping_) NimBLEDevice::getAdvertising()->start();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    mtu_ = mtu;
    LOG_INF("BUDDY", "MTU changed to %u", mtu);
  }

  void onIdentity(NimBLEConnInfo&) override { LOG_INF("BUDDY", "Peer identity resolved"); }

  void onConnParamsUpdate(NimBLEConnInfo&) override { LOG_INF("BUDDY", "Connection parameters updated"); }

  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subValue) override {
    LOG_INF("BUDDY", "TX subscription=%u encrypted=%d", subValue, info.isEncrypted());
  }

  void onRead(NimBLECharacteristic*, NimBLEConnInfo& info) override {
    LOG_INF("BUDDY", "GATT read encrypted=%d", info.isEncrypted());
  }

  uint32_t onPassKeyDisplay() override {
    passkey_ = esp_random() % 1000000;
    LOG_INF("BUDDY", "Passkey requested, host stack free %u B", uxTaskGetStackHighWaterMark(nullptr));
    return passkey_;
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    LOG_INF("BUDDY", "Auth complete: encrypted=%d bonded=%d, host stack free %u B", info.isEncrypted(), info.isBonded(),
            uxTaskGetStackHighWaterMark(nullptr));
    passkey_ = 0;
    if (!info.isEncrypted()) {
      LOG_ERR("BUDDY", "Pairing failed, disconnecting");
      server_->disconnect(info);
      return;
    }
    secure_ = true;
  }

  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    const NimBLEAttValue& value = chr->getValue();
    if (rxWriteLogs_ < 3) {
      ++rxWriteLogs_;
      LOG_INF("BUDDY", "RX write %u bytes encrypted=%d", static_cast<unsigned>(value.length()), info.isEncrypted());
    }
    if (!ringMutex_ || xSemaphoreTake(ringMutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;
    const uint8_t* data = value.data();
    for (size_t i = 0; i < value.length(); ++i) {
      const size_t next = (head_ + 1) % kRingBytes;
      if (next == tail_) {
        LOG_ERR("BUDDY", "RX ring full, dropping bytes");
        break;
      }
      ring_[head_] = data[i];
      head_ = next;
    }
    xSemaphoreGive(ringMutex_);
  }

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* rx_ = nullptr;
  NimBLECharacteristic* tx_ = nullptr;
  HeapByteBuffer ring_;
  SemaphoreHandle_t ringMutex_ = nullptr;
  size_t head_ = 0;
  size_t tail_ = 0;
  char name_[16] = {};
  bool started_ = false;
  uint8_t rxWriteLogs_ = 0;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> secure_{false};
  std::atomic<uint32_t> passkey_{0};
  std::atomic<uint16_t> mtu_{kDefaultMtu};
  std::atomic<uint16_t> connHandle_{0};
};

std::unique_ptr<BuddyTransport> makeBleBuddyTransport() { return makeUniqueNoThrow<BleNusPeripheral>(); }

#endif
