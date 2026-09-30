#include "StockTickerInfoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "StockWatchlistStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HeaderDateUtils.h"

namespace {
void appendWrapped(std::vector<std::string>& out, GfxRenderer& renderer, const std::string& text, int textWidth) {
  const auto wrapped = renderer.wrappedText(UI_10_FONT_ID, text.c_str(), textWidth, 64, EpdFontFamily::REGULAR);
  if (wrapped.empty()) {
    out.push_back(text);
  } else {
    out.insert(out.end(), wrapped.begin(), wrapped.end());
  }
  out.emplace_back("");
}
}  // namespace

void StockTickerInfoActivity::appendParagraph(const std::string& text, int textWidth) {
  appendWrapped(lines, renderer, text, textWidth);
}

void StockTickerInfoActivity::loadContent() {
  lines.clear();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int textWidth =
      renderer.getScreenWidth() - metrics.contentSidePadding * 2 - metrics.scrollBarWidth -
      metrics.scrollBarRightOffset - 8;

  char buf[96];
  snprintf(buf, sizeof(buf), "%s: Yahoo Finance", tr(STR_STOCK_DATA_SOURCE));
  appendWrapped(lines, renderer, buf, textWidth);

  StrId intervalId = StrId::STR_STOCK_REFRESH_AUTO;
  switch (SETTINGS.stockRefreshInterval) {
    case CrossPointSettings::STOCK_REFRESH_5M:
      intervalId = StrId::STR_WEB_DASH_INTERVAL_5M;
      break;
    case CrossPointSettings::STOCK_REFRESH_10M:
      intervalId = StrId::STR_STOCK_REFRESH_10M;
      break;
    case CrossPointSettings::STOCK_REFRESH_15M:
      intervalId = StrId::STR_WEB_DASH_INTERVAL_15M;
      break;
    case CrossPointSettings::STOCK_REFRESH_30M:
      intervalId = StrId::STR_WEB_DASH_INTERVAL_30M;
      break;
    case CrossPointSettings::STOCK_REFRESH_1H:
      intervalId = StrId::STR_STOCK_REFRESH_1H;
      break;
    case CrossPointSettings::STOCK_REFRESH_4H:
      intervalId = StrId::STR_STOCK_REFRESH_4H;
      break;
    case CrossPointSettings::STOCK_REFRESH_AUTO:
    default:
      break;
  }
  snprintf(buf, sizeof(buf), "%s: %s", tr(STR_STOCK_REFRESH_INTERVAL),
           I18n::getInstance().get(intervalId));
  appendWrapped(lines, renderer, buf, textWidth);

  snprintf(buf, sizeof(buf), "%s: %d", tr(STR_STOCK_SYMBOLS), STOCK_WATCHLIST.getCount());
  appendWrapped(lines, renderer, buf, textWidth);

  snprintf(buf, sizeof(buf), "%s: %s", tr(STR_STOCK_WATCHLIST), StockWatchlistStore::getFilePath());
  appendWrapped(lines, renderer, buf, textWidth);

  const char* source = STOCK_WATCHLIST.isFromFile() ? tr(STR_STOCK_SOURCE_FILE) : tr(STR_STOCK_SOURCE_BUILTIN);
  snprintf(buf, sizeof(buf), "%s: %s", tr(STR_STOCK_WATCHLIST), source);
  appendWrapped(lines, renderer, buf, textWidth);

  appendWrapped(lines, renderer, tr(STR_STOCK_EDIT_HINT), textWidth);
  lines.emplace_back("");

  appendWrapped(lines, renderer, tr(STR_STOCK_TICKER_DESC), textWidth);
  appendWrapped(lines, renderer, tr(STR_STOCK_HELP_1), textWidth);
  appendWrapped(lines, renderer, tr(STR_STOCK_HELP_2), textWidth);
  lines.emplace_back("");
  appendWrapped(lines, renderer, tr(STR_STOCK_JSON_HINT), textWidth);
  appendWrapped(lines, renderer, tr(STR_STOCK_JSON_RULES), textWidth);
  lines.emplace_back("");
  appendWrapped(lines, renderer, tr(STR_STOCK_OFFLINE_CACHED), textWidth);

  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  scrollOffset = std::clamp(scrollOffset, 0, getMaxScrollOffset());
}

int StockTickerInfoActivity::getVisibleLineCount() const {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const Rect bounds = theme.getScreenSafeArea(renderer, true, false);
  const int contentTop = bounds.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int viewportHeight = bounds.y + bounds.height - contentTop - metrics.verticalSpacing;
  return std::max(1, viewportHeight / lineHeight);
}

int StockTickerInfoActivity::getMaxScrollOffset() const {
  return std::max(0, static_cast<int>(lines.size()) - getVisibleLineCount());
}

void StockTickerInfoActivity::onEnter() {
  Activity::onEnter();
  loadContent();
  requestUpdate();
}

void StockTickerInfoActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int maxScrollOffset = getMaxScrollOffset();
    const int page = getVisibleLineCount();
    const int next =
        std::clamp(scrollOffset + (swipe == MappedInputManager::SwipeDir::Up ? page : -page), 0, maxScrollOffset);
    if (next != scrollOffset) {
      scrollOffset = next;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right, MappedInputManager::Button::Down}, [this] {
    const int maxScrollOffset = getMaxScrollOffset();
    if (maxScrollOffset <= 0) return;
    scrollOffset = std::min(scrollOffset + 1, maxScrollOffset);
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left, MappedInputManager::Button::Up}, [this] {
    if (scrollOffset <= 0) return;
    scrollOffset = std::max(0, scrollOffset - 1);
    requestUpdate();
  });
}

void StockTickerInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  const Rect bounds = theme.getScreenSafeArea(renderer, true, false);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int sidePadding = metrics.contentSidePadding;
  const int contentTop = bounds.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  HeaderDateUtils::drawHeaderWithDate(renderer, tr(STR_STOCK_INFO));

  const int viewportHeight = bounds.y + bounds.height - contentTop - metrics.verticalSpacing;
  const int visibleLines = std::max(1, viewportHeight / lineHeight);
  const int endLine = std::min(static_cast<int>(lines.size()), scrollOffset + visibleLines);

  int textY = contentTop;
  for (int i = scrollOffset; i < endLine; ++i) {
    if (!lines[i].empty()) {
      renderer.drawText(UI_10_FONT_ID, bounds.x + sidePadding, textY, lines[i].c_str());
    }
    textY += lineHeight;
  }

  if (static_cast<int>(lines.size()) > visibleLines) {
    const int maxScrollOffset = getMaxScrollOffset();
    if (maxScrollOffset > 0) {
      const int scrollTrackX = bounds.x + bounds.width - metrics.scrollBarRightOffset;
      const int scrollBarHeight = std::max(18, (viewportHeight * visibleLines) / static_cast<int>(lines.size()));
      const int scrollBarY =
          contentTop + ((viewportHeight - scrollBarHeight) * std::min(scrollOffset, maxScrollOffset)) / maxScrollOffset;
      renderer.drawLine(scrollTrackX, contentTop, scrollTrackX, contentTop + viewportHeight, true);
      renderer.fillRect(scrollTrackX - metrics.scrollBarWidth + 1, scrollBarY, metrics.scrollBarWidth, scrollBarHeight,
                        true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
