#include "AppOptionsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>
#include <string>
#include <vector>

#include "AppChrome.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace AppOptionFormat {

void orientation(const uint8_t index, char* out, const size_t outSize) {
  static constexpr StrId kLabels[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == CrossPointSettings::ORIENTATION_COUNT);
  snprintf(out, outSize, "%s", I18N.get(kLabels[index < CrossPointSettings::ORIENTATION_COUNT ? index : 0]));
}

void minutes(const uint16_t minutes, char* out, const size_t outSize) {
  if (minutes >= 60 && minutes % 60 == 0) {
    snprintf(out, outSize, tr(STR_HOURS_VALUE_FORMAT), static_cast<unsigned>(minutes / 60));
  } else {
    snprintf(out, outSize, tr(STR_SLEEP_TIMER_VALUE_FORMAT), static_cast<unsigned>(minutes));
  }
}

}  // namespace AppOptionFormat

namespace {
std::string choiceLabel(const AppOptionRow& row, const uint8_t index) {
  char buf[48] = "";
  if (row.formatChoice) row.formatChoice(index, buf, sizeof(buf));
  return buf;
}
}  // namespace

AppOptionsActivity::AppOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const StrId title,
                                       const AppOptionRow* rows, const int rowCount)
    : Activity("AppOptions", renderer, mappedInput),
      title_(title),
      rows_(rows),
      rowCount_(rowCount),
      uiTarget_(makeUiTarget(renderer)),
      app_(uiTarget_, uiTarget_.deviceContext()) {}

void AppOptionsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  topIndex_ = 0;
  changed_ = false;
  applySharedUiTheme(app_, uiTarget_);
  app_.on(ACTION_ROW, &AppOptionsActivity::onRowEvent, this);
  app_.setScreen(&AppOptionsActivity::optionsScreen, this);
  requestUpdate();
}

void AppOptionsActivity::onExit() {
  // Saved here rather than per change so a burst of edits costs one SD write.
  if (changed_ && !SETTINGS.saveToFile()) LOG_ERR("APPS", "Failed to save app options");
  Activity::onExit();
}

void AppOptionsActivity::activateSelected() {
  if (selectedIndex_ < 0 || selectedIndex_ >= rowCount_) return;
  const AppOptionRow& row = rows_[selectedIndex_];
  switch (row.kind) {
    case AppOptionRow::Kind::Toggle: {
      uint8_t& value = SETTINGS.*(row.field);
      value = value ? 0 : 1;
      changed_ = true;
      requestUpdate();
      break;
    }
    case AppOptionRow::Kind::Choice: {
      std::vector<std::string> labels;
      labels.reserve(row.choiceCount);
      for (uint8_t i = 0; i < row.choiceCount; i++) labels.push_back(choiceLabel(row, i));
      const uint8_t current = SETTINGS.*(row.field);
      optionPopup_.show(row.label, labels, current < row.choiceCount ? current : 0, [this, &row](const int index) {
        SETTINGS.*(row.field) = static_cast<uint8_t>(index);
        changed_ = true;
        requestUpdate();
      });
      requestUpdate();
      break;
    }
    case AppOptionRow::Kind::Screen:
      if (row.makeScreen) {
        startActivityForResult(row.makeScreen(renderer, mappedInput), [this](const ActivityResult&) {});
      }
      break;
  }
}

void AppOptionsActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (AppChrome::backTapped(mappedInput, renderer) || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (uiReady_) {
    const fui::InputSnapshot snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app_.route(snapshot);
      if (app_.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  buttonNavigator_.onNextRelease([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, rowCount_);
    topIndex_ = followListSelection(selectedIndex_, topIndex_, visibleRows_, rowCount_);
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, rowCount_);
    topIndex_ = followListSelection(selectedIndex_, topIndex_, visibleRows_, rowCount_);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) activateSelected();
}

void AppOptionsActivity::optionsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<AppOptionsActivity*>(user)->buildOptionsScreen(screen);
}

void AppOptionsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<AppOptionsActivity*>(user);
  if (self->optionPopup_.isActive() || event.value < 0 || event.value >= self->rowCount_) return;
  self->selectedIndex_ = event.value;
  self->app_.clearTapFlash();
  self->activateSelected();
}

void AppOptionsActivity::buildOptionsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Per-render owned value strings; items point into them for the draw only.
  std::vector<std::string> values(static_cast<size_t>(rowCount_));
  std::vector<fui::ListItem> items;
  items.reserve(static_cast<size_t>(rowCount_));
  for (int i = 0; i < rowCount_; i++) {
    const AppOptionRow& row = rows_[i];
    fui::ListItem item;
    item.label = I18N.get(row.label);
    if (row.kind == AppOptionRow::Kind::Choice) {
      values[i] = choiceLabel(row, SETTINGS.*(row.field));
      item.value = values[i].c_str();
    }
    item.toggle = row.kind == AppOptionRow::Kind::Toggle;
    item.toggleChecked = item.toggle && SETTINGS.*(row.field) != 0;
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows_ = rows > 0 ? rows : 1;
  topIndex_ = scrollListBy(topIndex_, 0, visibleRows_, rowCount_);
  props.topIndex = static_cast<uint16_t>(topIndex_);
  screen.list(props);
}

void AppOptionsActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  const Rect header = AppChrome::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, I18N.get(title_), false);
  } else {
    GUI.drawHeader(renderer, header, I18N.get(title_));
  }
  uiReady_ = false;
  app_.render();
  uiReady_ = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
