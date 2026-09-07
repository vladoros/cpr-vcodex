#pragma once

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class WebDashViewerActivity final : public Activity {
  enum class ViewState { Idle, Connecting, Downloading, Error };

  void startFetch();
  void beginWifiConnect();
  bool findSavedCredential(std::string& ssid, std::string& password);
  void connectAndDownload();
  void applyOrientation();
  void restoreOrientation();

  ButtonNavigator buttonNavigator;
  ViewState state = ViewState::Idle;
  bool fetching = false;
  bool wifiEnabledForActivity = false;
  bool wifiConnectedOnEnter = false;
  bool imageLoaded = false;
  bool errorState = false;
  std::string errorMessage;
  unsigned long lastFetchMs = 0;
  unsigned long connectStartMs = 0;
  GfxRenderer::Orientation originalOrientation = GfxRenderer::Portrait;
  bool orientationApplied = false;
  bool cancelling = false;

 public:
  explicit WebDashViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WebDashViewer", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  uint8_t getUiTransitionRefreshWeight() const override { return UI_TRANSITION_REFRESH_WEIGHT_DENSE; }
};
