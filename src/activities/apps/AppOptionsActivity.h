#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// One row of an app's Options page. Rows edit uint8_t SETTINGS fields directly.
struct AppOptionRow {
  enum class Kind : uint8_t { Choice, Toggle, Screen };

  Kind kind;
  StrId label;
  uint8_t CrossPointSettings::* field = nullptr;  // Choice / Toggle
  uint8_t choiceCount = 0;                        // Choice
  // Writes the label of choice `index` (Choice rows).
  void (*formatChoice)(uint8_t index, char* out, size_t outSize) = nullptr;
  // Opens a sub-screen (Screen rows).
  std::unique_ptr<Activity> (*makeScreen)(GfxRenderer& renderer, MappedInputManager& input) = nullptr;
};

// Shared value labels for app option rows.
namespace AppOptionFormat {
// CrossPointSettings::ORIENTATION index -> "Portrait", "Landscape CW", ...
void orientation(uint8_t index, char* out, size_t outSize);
// Minutes -> "5 min" / "4 h".
void minutes(uint16_t minutes, char* out, size_t outSize);
}  // namespace AppOptionFormat

// Settings list shared by the network apps. The caller re-reads SETTINGS when
// this screen finishes; changes are saved once on exit.
class AppOptionsActivity final : public Activity {
 public:
  AppOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId title, const AppOptionRow* rows,
                     int rowCount);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<12, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;

  StrId title_;
  const AppOptionRow* rows_;
  int rowCount_;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  int selectedIndex_ = 0;
  int visibleRows_ = 1;
  int topIndex_ = 0;
  bool changed_ = false;

  freeink::ui::GfxRendererTarget uiTarget_;  // must precede app_: the app holds a reference to it
  UiApp app_;
  std::atomic<bool> uiReady_{false};

  void activateSelected();
  static void optionsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildOptionsScreen(UiApp::ScreenType& screen);
};
