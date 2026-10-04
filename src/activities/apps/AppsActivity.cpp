#include "AppsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <vector>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#if CROSSINK_APP_CAP_CLAUDE_BUDDY
#include "claude_buddy/ClaudeBuddyActivity.h"
#endif

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr int appCount() { return static_cast<int>(kAppCount); }
}  // namespace

AppsActivity::AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Apps", renderer, mappedInput), ui(renderer) {}

void AppsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<AppsActivity*>(user);
  if (event.value < 0 || event.value >= appCount()) return;
  self->selectedIndex = event.value;
  self->ui.app.clearTapFlash();
#if CROSSINK_APP_HAS_APPS
  self->openApp(kApps[event.value].id);
#endif
}

void AppsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  buttonSelectionActive = !mappedInput.hasTouchHardware();
  ui.closeRouting();
  visibleRows = 1;
  topIndex = 0;
  listNav.reset(buttonSelectionActive ? selectedIndex : -1);
  ui.reset();
  ui.app.on(ACTION_ROW, &AppsActivity::onRowEvent, this);
  ui.app.setScreen(&AppsActivity::listScreen, this);
  requestUpdate();
}

void AppsActivity::exitToHome() {
  mappedInput.suppressNextBackRelease();
  activityManager.goHome(HomeMenuItem::APPS);
}

void AppsActivity::openApp(const AppId id) {
  std::unique_ptr<Activity> app;
  switch (id) {
    case AppId::ClaudeBuddy:
#if CROSSINK_APP_CAP_CLAUDE_BUDDY
      app = makeUniqueNoThrow<ClaudeBuddyActivity>(renderer, mappedInput);
#endif
      break;
  }
  if (!app) {
    LOG_ERR("APPS", "Cannot allocate app %u", static_cast<unsigned>(id));
    return;
  }
  startActivityForResult(std::move(app), [this](const ActivityResult&) { requestUpdate(); });
}

void AppsActivity::moveSelection(const bool forward) {
  if (appCount() == 0) return;
  {
    RenderLock lock(*this);
    selectedIndex = forward ? ButtonNavigator::nextIndex(selectedIndex, appCount())
                            : ButtonNavigator::previousIndex(selectedIndex, appCount());
    buttonSelectionActive = true;
    listNav.selected = selectedIndex;
    listNav.top = topIndex;
    listNav.visibleRows = visibleRows;
    listNav.follow(appCount());
    topIndex = listNav.top;
  }
  requestUpdate();
}

void AppsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitToHome();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
#if CROSSINK_APP_HAS_APPS
    if (selectedIndex >= 0 && selectedIndex < appCount()) openApp(kApps[selectedIndex].id);
#endif
    return;
  }

  if (ui.routingReady()) {
    fui::ActionEvent event{};
    if (ui.routeTouch(mappedInput, event)) {
      if (ui.app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  buttonNavigator.onNext([this] { moveSelection(true); });
  buttonNavigator.onPrevious([this] { moveSelection(false); });
}

void AppsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<AppsActivity*>(user)->buildListScreen(screen);
}

void AppsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  std::vector<fui::ListItem> items;
  items.reserve(kAppCount);
#if CROSSINK_APP_HAS_APPS
  for (int i = 0; i < appCount(); i++) {
    fui::ListItem item;
    item.label = I18N.get(kApps[i].name);
    item.subtitle = I18N.get(kApps[i].desc);
    item.icon = listIconFor(kApps[i].icon, 32);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
#endif

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.bold = false;
  props.subtitleText.maxLines = 2;
  props.rowGap = 10;
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  listNav.selected = buttonSelectionActive ? selectedIndex : -1;
  listNav.top = topIndex;
  listNav.visibleRows = visibleRows;
  listNav.syncToProps(screen.body(), props.rowHeight, props.rowGap, appCount(), props);
  topIndex = listNav.top;
  screen.list(props);
  topIndex = listNav.top;
}

void AppsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, ui.target, header, tr(STR_APPS), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_APPS));
  }

  ui.closeRouting();
  for (int pass = 0; pass < 8; ++pass) {
    ui.render();
    if (!listNav.consumeRebuildNeeded()) break;
  }

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(screenTransitionRefresh.modeFor(0));
}
