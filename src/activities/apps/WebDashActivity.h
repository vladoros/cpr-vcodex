#pragma once

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class WebDashActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void openUrlEditor();
  void openViewer();

 public:
  explicit WebDashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WebDash", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
