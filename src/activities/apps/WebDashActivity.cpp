#include "WebDashActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "WebDashViewerActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HeaderDateUtils.h"

namespace {
constexpr int ACTION_COUNT = 4;

std::string getRefreshIntervalLabel() {
  switch (SETTINGS.webDashRefreshInterval) {
    case CrossPointSettings::REFRESH_30S:
      return tr(STR_WEB_DASH_INTERVAL_30S);
    case CrossPointSettings::REFRESH_1M:
      return tr(STR_WEB_DASH_INTERVAL_1M);
    case CrossPointSettings::REFRESH_15M:
      return tr(STR_WEB_DASH_INTERVAL_15M);
    case CrossPointSettings::REFRESH_30M:
      return tr(STR_WEB_DASH_INTERVAL_30M);
    case CrossPointSettings::REFRESH_5M:
    default:
      return tr(STR_WEB_DASH_INTERVAL_5M);
  }
}

std::string getOrientationLabel() {
  return SETTINGS.webDashOrientation == CrossPointSettings::WD_LANDSCAPE
             ? std::string(tr(STR_WEB_DASH_LANDSCAPE))
             : std::string(tr(STR_WEB_DASH_PORTRAIT));
}

std::string getConfiguredUrlLabel() {
  return SETTINGS.webDashUrl[0] != '\0' ? std::string(SETTINGS.webDashUrl) : std::string(tr(STR_NOT_SET));
}
}  // namespace

void WebDashActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = std::clamp(selectedIndex, 0, ACTION_COUNT - 1);
  requestUpdate();
}

void WebDashActivity::onExit() { Activity::onExit(); }

void WebDashActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == 0) {
      openUrlEditor();
    } else if (selectedIndex == 1) {
      SETTINGS.webDashRefreshInterval =
          (SETTINGS.webDashRefreshInterval + 1) % CrossPointSettings::REFRESH_INTERVAL_COUNT;
      SETTINGS.saveToFile();
      requestUpdate();
    } else if (selectedIndex == 2) {
      SETTINGS.webDashOrientation =
          (SETTINGS.webDashOrientation + 1) % CrossPointSettings::WEB_DASH_ORIENTATION_COUNT;
      SETTINGS.saveToFile();
      requestUpdate();
    } else {
      openViewer();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ACTION_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ACTION_COUNT);
    requestUpdate();
  });
}

void WebDashActivity::openUrlEditor() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEB_DASH_URL),
                                              std::string(SETTINGS.webDashUrl), sizeof(SETTINGS.webDashUrl) - 1,
                                              InputType::Url),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& text = std::get<KeyboardResult>(result.data).text;
          strncpy(SETTINGS.webDashUrl, text.c_str(), sizeof(SETTINGS.webDashUrl) - 1);
          SETTINGS.webDashUrl[sizeof(SETTINGS.webDashUrl) - 1] = '\0';
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void WebDashActivity::openViewer() {
  if (SETTINGS.webDashUrl[0] == '\0') {
    // No URL configured - prompt the user to set one first.
    openUrlEditor();
    return;
  }
  startActivityForResult(std::make_unique<WebDashViewerActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void WebDashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  HeaderDateUtils::drawHeaderWithDate(renderer, tr(STR_WEB_DASH));

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ACTION_COUNT, selectedIndex,
      [](int index) {
        if (index == 0) return std::string(tr(STR_WEB_DASH_URL));
        if (index == 1) return std::string(tr(STR_WEB_DASH_INTERVAL));
        if (index == 2) return std::string(tr(STR_WEB_DASH_ORIENTATION));
        return std::string(tr(STR_WEB_DASH_OPEN));
      },
      [this](int index) {
        if (index == 0) return getConfiguredUrlLabel();
        if (index == 1) return getRefreshIntervalLabel();
        if (index == 2) return getOrientationLabel();
        return std::string();
      },
      [](int index) {
        if (index == 0) return Wifi;
        if (index == 1) return Recent;
        if (index == 2) return Settings;
        return Image;
      },
      nullptr, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
