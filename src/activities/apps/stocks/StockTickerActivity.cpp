#include "StockTickerActivity.h"

#if CROSSINK_APP_CAP_STOCKS

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <StockFormat.h>
#include <StockQuoteParser.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "StockCacheStore.h"
#include "StockTickerInfoActivity.h"
#include "StockWatchlistStore.h"
#include "activities/RenderLock.h"
#include "activities/apps/AppChrome.h"
#include "activities/apps/AppFonts.h"
#include "activities/apps/AppNetwork.h"
#include "activities/apps/AppOptionsActivity.h"
#include "activities/apps/AppRefreshInterval.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr uint8_t kFullRefreshEvery = 12;
constexpr size_t kResponseReserve = 2048;
// Yahoo occasionally fails one query host; the chart API is identical on
// query2, so it is a cheap retry before declaring the symbol failed.
constexpr const char* kYahooHosts[] = {"query1.finance.yahoo.com", "query2.finance.yahoo.com"};

void formatIntervalChoice(const uint8_t index, char* out, const size_t outSize) {
  const uint16_t minutes = AppRefreshInterval::minutesAt(AppRefreshInterval::kStockMinutes, index);
  if (minutes == 0) {
    snprintf(out, outSize, "%s", tr(STR_STOCK_REFRESH_AUTO));
  } else {
    AppOptionFormat::minutes(minutes, out, outSize);
  }
}

std::unique_ptr<Activity> makeInfoScreen(GfxRenderer& renderer, MappedInputManager& input) {
  return makeUniqueNoThrow<StockTickerInfoActivity>(renderer, input);
}

constexpr AppOptionRow kOptionRows[] = {
    {AppOptionRow::Kind::Choice, StrId::STR_REFRESH_INTERVAL, &CrossPointSettings::stockRefreshInterval,
     CrossPointSettings::APP_STOCK_INTERVAL_CHOICES, &formatIntervalChoice},
    {AppOptionRow::Kind::Choice, StrId::STR_ORIENTATION, &CrossPointSettings::stockOrientation,
     CrossPointSettings::ORIENTATION_COUNT, &AppOptionFormat::orientation},
    {AppOptionRow::Kind::Toggle, StrId::STR_STOCK_TOPBAR, &CrossPointSettings::stockTopbarEnabled},
    {AppOptionRow::Kind::Screen, StrId::STR_STOCK_INFO, nullptr, 0, nullptr, &makeInfoScreen},
};
constexpr int kOptionRowCount = static_cast<int>(sizeof(kOptionRows) / sizeof(kOptionRows[0]));

const char* marketStateLabel(const StockMarketState state) {
  switch (state) {
    case STOCK_MARKET_OPEN:
      return tr(STR_MARKET_OPEN);
    case STOCK_MARKET_PRE:
      return tr(STR_MARKET_PRE);
    case STOCK_MARKET_POST:
      return tr(STR_MARKET_POST);
    case STOCK_MARKET_CLOSED:
      break;
  }
  return tr(STR_MARKET_CLOSED);
}

int32_t nowEpoch() { return AppNetwork::clockValid() ? static_cast<int32_t>(time(nullptr)) : 0; }

void formatClock(const int32_t epoch, const int32_t offsetSeconds, char* out, const size_t outSize) {
  const time_t t = static_cast<time_t>(static_cast<int64_t>(epoch) + offsetSeconds);
  struct tm parts = {};
  gmtime_r(&t, &parts);
  snprintf(out, outSize, "%02d:%02d", parts.tm_hour, parts.tm_min);
}

// SETTINGS.clockUtcOffsetQ is quarter-hours biased by 48 (48 = UTC+0).
int32_t localOffsetSeconds() { return (static_cast<int32_t>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60; }

void formatUtcLabel(const int32_t offsetSeconds, char* out, const size_t outSize) {
  const int32_t minutes = offsetSeconds / 60;
  const int32_t absMinutes = minutes < 0 ? -minutes : minutes;
  if (minutes == 0) {
    snprintf(out, outSize, "UTC");
  } else if (absMinutes % 60 == 0) {
    snprintf(out, outSize, "UTC%c%d", minutes < 0 ? '-' : '+', static_cast<int>(absMinutes / 60));
  } else {
    snprintf(out, outSize, "UTC%c%d:%02d", minutes < 0 ? '-' : '+', static_cast<int>(absMinutes / 60),
             static_cast<int>(absMinutes % 60));
  }
}

bool anyValid(const StockQuote* quotes, const int count) {
  for (int i = 0; i < count; i++) {
    if (quotes[i].valid) return true;
  }
  return false;
}

void drawChangeTriangle(const GfxRenderer& renderer, const int cx, const int cy, const bool up) {
  const int xs[3] = {cx - 5, cx + 5, cx};
  const int ys[3] = {up ? cy + 4 : cy - 4, up ? cy + 4 : cy - 4, up ? cy - 4 : cy + 4};
  renderer.fillPolygon(xs, ys, 3, true);
}
}  // namespace

void StockTickerActivity::onEnter() {
  Activity::onEnter();
  // Each HTTPS handshake needs ~40 KB of internal heap; on a C3 the SD reader
  // font is the largest thing we can give back for the session.
  sdFontSystem.releaseForNetwork(renderer);
  originalOrientation_ = renderer.getOrientation();
  applyOrientation();
  priceFontId_ = appDisplayFontId(renderer, 14);
  changeFontId_ = appDisplayFontId(renderer, 12);

  response_.reserve(kResponseReserve);
  lastFetchMs_ = 0;
  pageIndex_ = 0;
  wifiOwned_ = false;
  topbarDirty_ = false;

  STOCK_WATCHLIST.ensureLoaded();
  STOCK_CACHE.ensureLoaded();
  loadWatchlist();
  cacheFetchTime_ = STOCK_CACHE.getLastFetchTime();
  // quotes_ now holds the cached rows; give the cache's copy back before TLS.
  STOCK_CACHE.release();
  startRefresh(/*userInitiated=*/true);
}

void StockTickerActivity::onExit() {
  if (wifiOwned_) AppNetwork::tearDown();
  // The header value is live in RAM; persist it once per session.
  if (topbarDirty_) SETTINGS.saveToFile();
  // ActivityManager holds the render lock around onExit.
  renderer.setOrientation(originalOrientation_);
  std::string().swap(response_);
  Activity::onExit();
}

void StockTickerActivity::applyOrientation() {
  RenderLock lock(*this);
  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(
      SETTINGS.stockOrientation < CrossPointSettings::ORIENTATION_COUNT ? SETTINGS.stockOrientation : 0));
  forceHalfRefresh_ = true;
}

void StockTickerActivity::exitApp() { finishAfterBackPress(); }

void StockTickerActivity::loadWatchlist() {
  quoteCount_ = STOCK_WATCHLIST.getCount();
  showingCache_ = false;
  const StockQuote* cached = STOCK_CACHE.getQuotes();
  const int cachedCount = STOCK_CACHE.getQuoteCount();
  for (int i = 0; i < quoteCount_; i++) {
    quotes_[i] = StockQuote{};
    snprintf(quotes_[i].symbol, sizeof(quotes_[i].symbol), "%s", STOCK_WATCHLIST.getSymbol(i));
    // Start from the last known snapshot so the screen is never blank while
    // the sweep runs; the cache may be ordered differently from the list.
    for (int c = 0; c < cachedCount; c++) {
      if (strcmp(cached[c].symbol, quotes_[i].symbol) == 0) {
        quotes_[i] = cached[c];
        quotes_[i].stale = true;
        showingCache_ = true;
        break;
      }
    }
  }
}

void StockTickerActivity::startRefresh(const bool userInitiated) {
  userRequestedFetch_ = userInitiated;
  if (AppNetwork::isConnected()) {
    beginSweep();
    return;
  }
  if (AppNetwork::beginSilentConnect()) {
    wifiOwned_ = true;
    connectStartMs_ = millis();
    state_ = State::Connecting;
    requestUpdate(true);
    return;
  }
  onConnectFailed(userInitiated);
}

void StockTickerActivity::onConnectFailed(const bool userInitiated) {
  const bool haveRows = anyValid(quotes_, quoteCount_);
  // Timer refreshes never pop the Wi-Fi picker; nor does entering with cached rows.
  if (!userInitiated || (haveRows && lastFetchMs_ == 0)) {
    lastFetchMs_ = millis();
    state_ = haveRows ? State::Displaying : State::Error;
    requestUpdate(true);
    return;
  }
  openWifiSelection();
}

void StockTickerActivity::openWifiSelection() {
  wifiOwned_ = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled && AppNetwork::isConnected()) {
                             beginSweep();
                             return;
                           }
                           lastFetchMs_ = millis();
                           if (!anyValid(quotes_, quoteCount_)) {
                             finish();
                             return;
                           }
                           state_ = State::Displaying;
                         });
}

void StockTickerActivity::beginSweep() {
  fetchIndex_ = -1;  // -1 = clock check pending before the first symbol
  sweepHadFailure_ = false;
  lastFetchMs_ = millis();
  state_ = State::Fetching;
  requestUpdate(true);
}

void StockTickerActivity::fetchNextSymbol() {
  StockQuote& row = quotes_[fetchIndex_];
  char encoded[STOCK_SYMBOL_MAX_LEN * 3 + 1];
  bool fetched = false;
  if (!StockFormat::stockUrlEncodeSymbol(row.symbol, encoded, sizeof(encoded))) {
    LOG_ERR("STOCK", "Cannot encode symbol %s", row.symbol);
  } else if (AppNetwork::hasTlsHeadroom("STOCK")) {
    char url[160];
    for (const char* host : kYahooHosts) {
      snprintf(url, sizeof(url), "https://%s/v8/finance/chart/%s?interval=1d&range=1d", host, encoded);
      if (!HttpDownloader::fetchUrl(url, response_)) {
        LOG_ERR("STOCK", "Fetch failed for %s via %s", row.symbol, host);
        continue;
      }
      StockQuote parsed;
      if (!StockQuoteParser::parse(response_.data(), response_.size(), row.symbol, parsed)) {
        LOG_ERR("STOCK", "Parse failed for %s via %s", row.symbol, host);
        continue;
      }
      RenderLock lock(*this);  // render() may be drawing this row
      row = parsed;
      fetched = true;
      break;
    }
  }
  // A failed row keeps its cached value, marked stale; a row never fetched stays invalid.
  if (!fetched) {
    RenderLock lock(*this);
    row.stale = true;
    sweepHadFailure_ = true;
  }
}

void StockTickerActivity::finishSweep() {
  const int32_t now = nowEpoch();
  if (!STOCK_CACHE.update(quotes_, quoteCount_, now)) LOG_ERR("STOCK", "Failed to save quote cache");
  STOCK_CACHE.release();
  // The footer's "cached" time stays at the last sweep where every row loaded.
  if (!sweepHadFailure_) cacheFetchTime_ = now;
  updateTopbarValue();
  showingCache_ = sweepHadFailure_;
  lastFetchMs_ = millis();
  state_ = anyValid(quotes_, quoteCount_) ? State::Displaying : State::Error;
  requestUpdate(true);
}

void StockTickerActivity::updateTopbarValue() {
  for (int i = 0; i < quoteCount_; i++) {
    const StockQuote& q = quotes_[i];
    if (!q.valid || q.stale || q.symbol[0] != '^') continue;
    SETTINGS.stockTopbarValue = q.price;
    SETTINGS.stockTopbarChangePct = q.changePercent;
    SETTINGS.stockTopbarValid = 1;
    topbarDirty_ = true;
    return;
  }
}

uint32_t StockTickerActivity::refreshIntervalMs() const {
  uint16_t minutes = AppRefreshInterval::minutesAt(AppRefreshInterval::kStockMinutes, SETTINGS.stockRefreshInterval);
  if (minutes == 0) {
    // Auto: the feed's delay tracks the exchange session, so poll often while
    // something is open and back off when everything is shut.
    const StockMarketSummary summary = StockFormat::stockComputeSummary(quotes_, quoteCount_, nowEpoch());
    const bool allClosed = summary.primary == STOCK_MARKET_CLOSED && summary.mixedCount == 0;
    minutes = allClosed ? AppRefreshInterval::kStockAutoClosedMinutes : AppRefreshInterval::kStockAutoOpenMinutes;
  }
  return AppRefreshInterval::minutesToMs(minutes);
}

int StockTickerActivity::totalPages() const {
  const int perPage = std::max(1, rowsPerPage_.load());
  return std::max(1, (quoteCount_ + perPage - 1) / perPage);
}

void StockTickerActivity::changePage(const int delta) {
  const int pages = totalPages();
  if (pages <= 1) return;
  pageIndex_ = (pageIndex_ + delta + pages) % pages;
  requestUpdate();
}

void StockTickerActivity::openOptions() {
  const uint8_t orientation = SETTINGS.stockOrientation;
  auto options = makeUniqueNoThrow<AppOptionsActivity>(renderer, mappedInput, StrId::STR_APP_OPTIONS, kOptionRows,
                                                       kOptionRowCount);
  if (!options) {
    LOG_ERR("STOCK", "Cannot allocate options screen");
    return;
  }
  startActivityForResult(std::move(options), [this, orientation](const ActivityResult&) {
    if (SETTINGS.stockOrientation != orientation) applyOrientation();
  });
}

void StockTickerActivity::loop() {
  if (AppChrome::backTapped(mappedInput, renderer) || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitApp();
    return;
  }

  switch (state_) {
    case State::Connecting:
      if (AppNetwork::isConnected()) {
        beginSweep();
      } else if (millis() - connectStartMs_ >= AppNetwork::kSilentConnectTimeoutMs) {
        LOG_ERR("STOCK", "Silent Wi-Fi connect timed out");
        AppNetwork::tearDown();
        onConnectFailed(userRequestedFetch_);
      }
      return;
    case State::Fetching:
      if (fetchIndex_ < 0) {
        // Paint the status first: the NTP sync below can block for seconds,
        // and HTTPS certificate checks need a valid clock.
        requestUpdateAndWait();
        AppNetwork::ensureClock();
        fetchIndex_ = 0;
      } else if (fetchIndex_ < quoteCount_) {
        fetchNextSymbol();
        fetchIndex_++;
        requestUpdate();
      } else {
        finishSweep();
      }
      return;
    case State::Displaying:
    case State::Error:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startRefresh(/*userInitiated=*/true);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openOptions();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    changePage(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    changePage(-1);
    return;
  }
  switch (mappedInput.wasSwipe()) {
    case MappedInputManager::SwipeDir::Left:
      changePage(1);
      return;
    case MappedInputManager::SwipeDir::Right:
      changePage(-1);
      return;
    case MappedInputManager::SwipeDir::Down:
      startRefresh(/*userInitiated=*/true);
      return;
    default:
      break;
  }
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    openOptions();
    return;
  }

  if (lastFetchMs_ != 0 && millis() - lastFetchMs_ >= refreshIntervalMs()) {
    startRefresh(/*userInitiated=*/false);
  }
}

void StockTickerActivity::drawStatusLine(const Rect& area, const int y) const {
  char buf[96];
  if (state_ == State::Connecting || state_ == State::Fetching) {
    if (state_ == State::Fetching && fetchIndex_ >= 0 && quoteCount_ > 0) {
      snprintf(buf, sizeof(buf), "%s %d/%d", tr(STR_FETCHING_STOCKS), std::min(fetchIndex_ + 1, quoteCount_),
               quoteCount_);
    } else {
      snprintf(buf, sizeof(buf), "%s", tr(STR_FETCHING_STOCKS));
    }
  } else {
    const StockMarketSummary summary = StockFormat::stockComputeSummary(quotes_, quoteCount_, nowEpoch());
    const char* tz = "";
    for (int i = 0; i < quoteCount_; i++) {
      if (quotes_[i].valid && quotes_[i].tzAbbr[0] != '\0') {
        tz = quotes_[i].tzAbbr;
        break;
      }
    }
    const char* sep = tz[0] != '\0' ? " \xC2\xB7 " : "";
    if (summary.mixedCount > 0) {
      snprintf(buf, sizeof(buf), "%s%s%s \xC2\xB7 %d/%d %s", marketStateLabel(summary.primary), sep, tz,
               summary.mixedCount, summary.totalCount, tr(STR_STOCK_MARKETS_DIFFER));
    } else {
      snprintf(buf, sizeof(buf), "%s%s%s", marketStateLabel(summary.primary), sep, tz);
    }
  }
  UITheme::drawCenteredText(renderer, area, UI_10_FONT_ID, y, buf);
}

void StockTickerActivity::drawRows(const Rect& area, const int top, const int rowHeight, const int rows) const {
  char priceBuf[32];
  char signedChange[24];
  char changeBuf[48];
  const int priceHeight = renderer.getLineHeight(priceFontId_);
  const int changeHeight = renderer.getLineHeight(changeFontId_);
  const int leftX = area.x;
  const int rightX = area.x + area.width;
  const int first = pageIndex_ * rows;

  for (int i = 0; i < rows && first + i < quoteCount_; i++) {
    const StockQuote& q = quotes_[first + i];
    const int y = top + i * rowHeight;
    renderer.drawText(priceFontId_, leftX, y, q.symbol, true, EpdFontFamily::BOLD);
    if (!q.valid) {
      renderer.drawText(changeFontId_, leftX, y + priceHeight, "--");
      continue;
    }

    StockFormat::stockFormatNumber(q.price, 2, priceBuf, sizeof(priceBuf));
    const int priceWidth = renderer.getTextWidth(priceFontId_, priceBuf, EpdFontFamily::BOLD);
    renderer.drawText(priceFontId_, rightX - priceWidth, y, priceBuf, true, EpdFontFamily::BOLD);

    StockFormat::stockFormatSigned(q.change, 2, signedChange, sizeof(signedChange));
    char percent[24];
    StockFormat::stockFormatPercent(q.changePercent, percent, sizeof(percent));
    snprintf(changeBuf, sizeof(changeBuf), "%s (%s)%s", signedChange, percent, q.stale ? " *" : "");
    const int changeWidth = renderer.getTextWidth(changeFontId_, changeBuf);
    const int changeX = rightX - changeWidth;
    const int changeY = y + priceHeight;
    renderer.drawText(changeFontId_, changeX, changeY, changeBuf);
    if (q.change != 0.0f) {
      drawChangeTriangle(renderer, std::max(leftX + 6, changeX - 12), changeY + changeHeight / 2, q.change > 0.0f);
    }
    if (i + 1 < rows && first + i + 1 < quoteCount_) {
      const int ruleY = y + rowHeight - 5;
      renderer.drawLine(leftX, ruleY, rightX, ruleY, true);
    }
  }
}

void StockTickerActivity::drawFooter(const Rect& area, const int y) const {
  renderer.fillRect(area.x, y - 6, area.width, 2, true);

  const int32_t epoch = nowEpoch();
  const int32_t localOffset = localOffsetSeconds();
  char left[96] = "";
  char localTz[16];
  formatUtcLabel(localOffset, localTz, sizeof(localTz));
  if (showingCache_ && cacheFetchTime_ > 0) {
    // Some rows are stale (marked *): say when the cached quotes were fetched.
    char cachedTime[8];
    formatClock(cacheFetchTime_, localOffset, cachedTime, sizeof(cachedTime));
    char stamp[24];
    snprintf(stamp, sizeof(stamp), "%s %s", cachedTime, localTz);
    snprintf(left, sizeof(left), tr(STR_CACHED_AT), stamp);
  } else if (epoch > 0) {
    char localTime[8];
    formatClock(epoch, localOffset, localTime, sizeof(localTime));
    const StockQuote* ref = nullptr;
    for (int i = 0; i < quoteCount_ && ref == nullptr; i++) {
      if (quotes_[i].valid) ref = &quotes_[i];
    }
    if (ref != nullptr) {
      char exchangeTime[8];
      formatClock(epoch, ref->gmtoffset, exchangeTime, sizeof(exchangeTime));
      // Quote age tracks the exchange session, not the fetch: a closed market
      // shows hours even right after a refresh.
      char age[16] = "";
      if (ref->quoteTime > 0 && epoch > ref->quoteTime) {
        const int32_t delta = epoch - ref->quoteTime;
        if (delta < 3600) {
          snprintf(age, sizeof(age), " \xC2\xB7 %dm", static_cast<int>(delta / 60));
        } else if (delta < 48 * 3600) {
          snprintf(age, sizeof(age), " \xC2\xB7 %dh", static_cast<int>(delta / 3600));
        } else {
          snprintf(age, sizeof(age), " \xC2\xB7 %dd", static_cast<int>(delta / 86400));
        }
      }
      snprintf(left, sizeof(left), "%s %s \xC2\xB7 %s %s%s", localTime, localTz, exchangeTime, ref->tzAbbr, age);
    } else {
      snprintf(left, sizeof(left), "%s %s", localTime, localTz);
    }
  }

  int leftMax = area.width;
  const int pages = totalPages();
  if (pages > 1) {
    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "%d/%d", pageIndex_ + 1, pages);
    const int pageWidth = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
    renderer.drawText(SMALL_FONT_ID, area.x + area.width - pageWidth, y, pageBuf);
    leftMax -= pageWidth + 12;
  }
  if (left[0] != '\0' && leftMax > 0) {
    const std::string clipped = renderer.truncatedText(SMALL_FONT_ID, left, leftMax);
    renderer.drawText(SMALL_FONT_ID, area.x, y, clipped.c_str());
  }
}

void StockTickerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect area = AppChrome::drawHeader(renderer, mappedInput, tr(STR_STOCK_TICKER));

  drawStatusLine(area, area.y);
  const int rowsTop = area.y + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  const int footerY = area.y + area.height - renderer.getLineHeight(SMALL_FONT_ID);
  const int rowHeight = renderer.getLineHeight(priceFontId_) + renderer.getLineHeight(changeFontId_) + 10;
  const int rows = std::max(1, (footerY - 10 - rowsTop) / rowHeight);
  rowsPerPage_.store(rows);
  const int pages = totalPages();
  if (pageIndex_ >= pages) pageIndex_ = pages - 1;

  if (anyValid(quotes_, quoteCount_) || state_ == State::Fetching) {
    drawRows(area, rowsTop, rowHeight, rows);
  } else if (state_ == State::Error) {
    UITheme::drawCenteredWrappedText(renderer, area, UI_12_FONT_ID, area.y + area.height / 3, tr(STR_STOCK_ERROR), 3,
                                     true, EpdFontFamily::BOLD);
  }
  drawFooter(area, footerY);

  const bool busy = state_ == State::Connecting || state_ == State::Fetching;
  const auto hints = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), busy ? "" : tr(STR_REFRESH),
                                           busy ? "" : tr(STR_APP_OPTIONS), pages > 1 ? tr(STR_NEXT) : "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4, true);

  HalDisplay::RefreshMode mode = screenTransitionRefresh_.modeFor(static_cast<uint8_t>(pageIndex_));
  if (forceHalfRefresh_) mode = HalDisplay::HALF_REFRESH;
  forceHalfRefresh_ = false;
  if (mode == HalDisplay::FAST_REFRESH && ++fastRefreshCount_ >= kFullRefreshEvery) mode = HalDisplay::HALF_REFRESH;
  if (mode != HalDisplay::FAST_REFRESH) fastRefreshCount_ = 0;
  renderer.displayBuffer(mode);
}

#endif
