#include "StockTickerInfoActivity.h"

#if CROSSINK_APP_CAP_STOCKS

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "StockWatchlistStore.h"
#include "activities/apps/AppChrome.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMaxParagraphLines = 24;

// Text column inside the app frame, leaving room for the scroll bar.
Rect textArea(const Rect& frame) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int scrollReserve = metrics.scrollBarWidth + metrics.scrollBarRightOffset + 8;
  return Rect{frame.x, frame.y, std::max(1, frame.width - scrollReserve), frame.height};
}
}  // namespace

void StockTickerInfoActivity::onEnter() {
  Activity::onEnter();
  STOCK_WATCHLIST.ensureLoaded();
  scrollOffset_ = 0;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  buildLines(textArea(Rect{0, 0, safe.width - metrics.contentSidePadding * 2, 1}).width);
  requestUpdate();
}

void StockTickerInfoActivity::onExit() {
  std::vector<std::string>().swap(lines_);
  Activity::onExit();
}

void StockTickerInfoActivity::buildLines(const int textWidth) {
  lines_.clear();
  lines_.reserve(48);
  const auto paragraph = [&](const char* text) {
    const auto wrapped = renderer.wrappedText(UI_10_FONT_ID, text, textWidth, kMaxParagraphLines);
    lines_.insert(lines_.end(), wrapped.begin(), wrapped.end());
    lines_.emplace_back();
  };

  char buf[160];
  snprintf(buf, sizeof(buf), "%s: Yahoo Finance", tr(STR_STOCK_DATA_SOURCE));
  paragraph(buf);
  snprintf(buf, sizeof(buf), "%s: %d (%s)", tr(STR_STOCK_SYMBOLS), STOCK_WATCHLIST.getCount(),
           STOCK_WATCHLIST.isFromFile() ? tr(STR_STOCK_SOURCE_FILE) : tr(STR_STOCK_SOURCE_BUILTIN));
  paragraph(buf);
  snprintf(buf, sizeof(buf), "%s: %s", tr(STR_STOCK_WATCHLIST), StockWatchlistStore::getFilePath());
  paragraph(buf);
  paragraph(tr(STR_STOCK_EDIT_HINT));
  paragraph(tr(STR_STOCK_JSON_HINT));
  paragraph(tr(STR_STOCK_JSON_RULES));
  paragraph(tr(STR_STOCK_HELP_1));
  paragraph(tr(STR_STOCK_HELP_2));
  paragraph(tr(STR_STOCK_HELP_3));
  while (!lines_.empty() && lines_.back().empty()) lines_.pop_back();
}

void StockTickerInfoActivity::scrollBy(const int delta) {
  const int maxOffset = std::max(0, static_cast<int>(lines_.size()) - visibleLines_);
  const int next = std::clamp(scrollOffset_ + delta, 0, maxOffset);
  if (next == scrollOffset_) return;
  scrollOffset_ = next;
  requestUpdate();
}

void StockTickerInfoActivity::loop() {
  if (AppChrome::backTapped(mappedInput, renderer) || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    scrollBy(swipe == MappedInputManager::SwipeDir::Up ? visibleLines_ : -visibleLines_);
    return;
  }
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Right, MappedInputManager::Button::Down},
                                        [this] { scrollBy(1); });
  buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Left, MappedInputManager::Button::Up},
                                        [this] { scrollBy(-1); });
}

void StockTickerInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect area = textArea(AppChrome::drawHeader(renderer, mappedInput, tr(STR_STOCK_INFO)));
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  visibleLines_ = std::max(1, area.height / lineHeight);
  const int total = static_cast<int>(lines_.size());
  scrollOffset_ = std::clamp(scrollOffset_, 0, std::max(0, total - visibleLines_));
  const int end = std::min(total, scrollOffset_ + visibleLines_);
  for (int i = scrollOffset_; i < end; i++) {
    if (!lines_[i].empty()) {
      renderer.drawText(UI_10_FONT_ID, area.x, area.y + (i - scrollOffset_) * lineHeight, lines_[i].c_str());
    }
  }

  if (total > visibleLines_) {
    const int trackX = area.x + area.width + 8 + metrics.scrollBarRightOffset;
    const int thumbHeight = std::max(18, area.height * visibleLines_ / total);
    const int maxOffset = total - visibleLines_;
    const int thumbY = area.y + (area.height - thumbHeight) * scrollOffset_ / maxOffset;
    renderer.drawLine(trackX, area.y, trackX, area.y + area.height, true);
    renderer.fillRect(trackX - metrics.scrollBarWidth + 1, thumbY, metrics.scrollBarWidth, thumbHeight, true);
  }

  const auto hints =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4, true);
  renderer.displayBuffer();
}

#endif
