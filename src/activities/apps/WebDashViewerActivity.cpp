#include "WebDashViewerActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/HeaderDateUtils.h"
#include "util/PngSleepRenderer.h"
#include "util/RefreshInterval.h"

namespace {
constexpr char WEB_DASH_IMAGE_PATH[] = "/.crosspoint/webdash.png";
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
}  // namespace

void WebDashViewerActivity::applyOrientation() {
  const GfxRenderer::Orientation orientation =
      SETTINGS.webDashOrientation == CrossPointSettings::WD_LANDSCAPE
          ? GfxRenderer::LandscapeCounterClockwise
          : GfxRenderer::Portrait;
  renderer.setOrientation(orientation);
}

void WebDashViewerActivity::restoreOrientation() {
  if (orientationApplied) {
    renderer.setOrientation(originalOrientation);
    renderer.requestNextFullRefresh();
    orientationApplied = false;
  }
}

void WebDashViewerActivity::onEnter() {
  Activity::onEnter();
  originalOrientation = renderer.getOrientation();
  applyOrientation();
  orientationApplied = true;
  renderer.requestNextFullRefresh();

  WIFI_STORE.loadFromFile();
  wifiConnectedOnEnter = WiFi.status() == WL_CONNECTED;
  wifiEnabledForActivity = false;
  cancelling = false;
  imageLoaded = false;
  errorState = false;
  state = ViewState::Idle;
  fetching = false;
  lastFetchMs = 0;
  requestUpdate(true);
  startFetch();
}

void WebDashViewerActivity::onExit() {
  cancelling = true;
  restoreOrientation();
  if (!wifiConnectedOnEnter && wifiEnabledForActivity) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  Activity::onExit();
}

bool WebDashViewerActivity::findSavedCredential(std::string& ssid, std::string& password) {
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    return false;
  }

  const std::optional<WifiCredential> cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    return false;
  }

  ssid = cred->ssid;
  password = cred->password;
  return true;
}

void WebDashViewerActivity::beginWifiConnect() {
  std::string ssid;
  std::string password;
  if (!findSavedCredential(ssid, password)) {
    state = ViewState::Error;
    errorMessage = tr(STR_WEB_DASH_NO_WIFI);
    fetching = false;
    requestUpdate(true);
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());

  if (password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
  connectStartMs = millis();
  state = ViewState::Connecting;
  wifiEnabledForActivity = true;
  requestUpdate(true);
}

void WebDashViewerActivity::startFetch() {
  if (fetching) {
    return;
  }
  fetching = true;
  cancelling = false;
  errorState = false;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnectedOnEnter) {
      wifiEnabledForActivity = true;
    }
    connectAndDownload();
  } else {
    beginWifiConnect();
  }
}

void WebDashViewerActivity::connectAndDownload() {
  state = ViewState::Downloading;
  requestUpdate(true);

  Storage.mkdir("/.crosspoint");

  const HttpDownloader::DownloadError result = HttpDownloader::downloadToFile(
      SETTINGS.webDashUrl, WEB_DASH_IMAGE_PATH, nullptr, &cancelling);

  if (cancelling) {
    fetching = false;
    state = ViewState::Idle;
    return;
  }

  if (result != HttpDownloader::OK) {
    state = ViewState::Error;
    errorMessage = tr(STR_WEB_DASH_ERROR);
    fetching = false;
    requestUpdate(true);
    return;
  }

  // Decode the fetched PNG directly into the framebuffer.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const bool decoded = PngSleepRenderer::drawTransparentPng(WEB_DASH_IMAGE_PATH, renderer, 0, 0, screenW, screenH);
  if (!decoded) {
    state = ViewState::Error;
    errorMessage = tr(STR_WEB_DASH_ERROR);
    fetching = false;
    requestUpdate(true);
    return;
  }

  imageLoaded = true;
  errorState = false;
  state = ViewState::Idle;
  fetching = false;
  lastFetchMs = millis();
  renderer.requestNextRefresh(HalDisplay::FULL_REFRESH);
  requestUpdate(true);
}

void WebDashViewerActivity::loop() {
  if (state == ViewState::Connecting) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      connectAndDownload();
      return;
    }
    if (millis() - connectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
      state = ViewState::Error;
      errorMessage = tr(STR_WEB_DASH_WIFI_FAILED);
      fetching = false;
      requestUpdate(true);
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (state == ViewState::Idle || state == ViewState::Error) {
      startFetch();
    }
    return;
  }

  if (state == ViewState::Idle && !fetching) {
    const unsigned long intervalMs = refreshIntervalMs(SETTINGS.webDashRefreshInterval);
    if (lastFetchMs != 0 && millis() - lastFetchMs >= intervalMs) {
      startFetch();
    }
  }
}

void WebDashViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // If already loaded, the last decoded image is already in the framebuffer.
  // The render task redraws from scratch each time, so we redraw the PNG from
  // disk if we have one; otherwise draw status text.
  if (imageLoaded) {
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    if (PngSleepRenderer::drawTransparentPng(WEB_DASH_IMAGE_PATH, renderer, 0, 0, screenW, screenH)) {
      // Draw a small status bar while refreshing.
      if (state == ViewState::Downloading) {
        renderer.drawText(UI_10_FONT_ID, 4, 4, tr(STR_WEB_DASH_REFRESH));
      }
      renderer.displayBuffer();
      return;
    }
  }

  if (state == ViewState::Error) {
    HeaderDateUtils::drawHeaderWithDate(renderer, tr(STR_WEB_DASH));
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, errorMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 8, tr(STR_WEB_DASH_RETRY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEB_DASH_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  HeaderDateUtils::drawHeaderWithDate(renderer, tr(STR_WEB_DASH));
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_WEB_DASH_LOADING), true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WEB_DASH_REFRESH), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
