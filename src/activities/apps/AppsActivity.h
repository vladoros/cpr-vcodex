#pragma once

#include "AppRegistry.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

class AppsActivity final : public Activity {
  using UiHost = UiAppHost<12, 4>;
  using UiApp = UiHost::App;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool buttonSelectionActive = false;

  UiHost ui;
  int visibleRows = 1;
  int topIndex = 0;
  freeink::ui::ListNav listNav;
  ScreenTransitionRefresh screenTransitionRefresh;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  void moveSelection(bool forward);
  void openApp(AppId id);
  void exitToHome();

 public:
  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
