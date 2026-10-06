#include "AppNetwork.h"

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <WiFi.h>

#include <ctime>
#include <optional>

#include "WifiCredentialStore.h"

namespace AppNetwork {

namespace {
// 2023-11-14; any earlier reading means the clock was never set.
constexpr time_t kMinValidEpoch = 1700000000;
}  // namespace

bool isConnected() { return WiFi.status() == WL_CONNECTED; }

bool hasSavedNetwork() {
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  return !ssid.empty() && WIFI_STORE.findCredential(ssid).has_value();
}

bool beginSilentConnect() {
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) return false;
  const std::optional<WifiCredential> cred = WIFI_STORE.findCredential(ssid);
  if (!cred) return false;

  LOG_INF("APPNET", "Connecting to saved network %s", ssid.c_str());
  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect.
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("APPNET", "Failed to enter station mode");
    return false;
  }
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  return true;
}

void tearDown() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

bool clockValid() { return time(nullptr) >= kMinValidEpoch; }

bool ensureClock() {
  if (clockValid()) return true;
#ifndef SIMULATOR
  if (isConnected() && !halClock.syncSystemTimeFromNTP()) {
    LOG_ERR("APPNET", "NTP sync failed; quote ages and HTTPS may be unavailable");
  }
#endif
  return clockValid();
}

bool hasTlsHeadroom(const char* tag) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  LOG_DBG("APPNET", "[%s] free heap %u, max alloc %u", tag, static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(maxAlloc));
  if (freeHeap >= kMinTlsFreeHeap && maxAlloc >= kMinTlsMaxAlloc) return true;
  LOG_ERR("APPNET", "[%s] skipping HTTPS: free heap %u / max alloc %u below floor", tag,
          static_cast<unsigned>(freeHeap), static_cast<unsigned>(maxAlloc));
  return false;
}

}  // namespace AppNetwork
