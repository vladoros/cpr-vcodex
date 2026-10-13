#include "StockTickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>

#include "CrossPointSettings.h"
#include "StockCacheStore.h"
#include "StockFormat.h"
#include "StockQuoteParser.h"
#include "StockWatchlistStore.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/NetworkMemory.h"
#include "util/TimeUtils.h"

namespace {
constexpr const char* YAHOO_HOST = "query1.finance.yahoo.com";
// Yahoo occasionally rate-limits or fails one query host; the chart API is
// identical on query2, so it is a cheap retry before giving up on the symbol.
constexpr const char* YAHOO_FALLBACK_HOST = "query2.finance.yahoo.com";
constexpr const char* YAHOO_PATH = "/v8/finance/chart/";
constexpr const char* YAHOO_QUERY = "?interval=1d&range=1d";
// Yahoo's chart endpoint 429s without a User-Agent. This is load-bearing:
// do not "clean it up".

constexpr int STOCK_PRICE_FONT = NOTOSANS_14_FONT_ID;
constexpr int STOCK_CHANGE_FONT = NOTOSANS_12_FONT_ID;
constexpr int STOCK_STATUS_FONT = UI_10_FONT_ID;

void drawTriangleUp(GfxRenderer& r, int cx, int y) {
  r.drawLine(cx - 4, y + 6, cx, y, true);
  r.drawLine(cx, y, cx + 4, y + 6, true);
  r.drawLine(cx - 4, y + 6, cx + 4, y + 6, true);
}

void drawTriangleDown(GfxRenderer& r, int cx, int y) {
  r.drawLine(cx - 4, y, cx, y + 6, true);
  r.drawLine(cx, y + 6, cx + 4, y, true);
  r.drawLine(cx - 4, y, cx + 4, y, true);
}

void applyOrientation(GfxRenderer& renderer) {
  switch (SETTINGS.stockOrientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}
}  // namespace

// --- lifecycle -------------------------------------------------------------

void StockTickerActivity::onEnter() {
  Activity::onEnter();
  originalOrientation = renderer.getOrientation();
  applyOrientation(renderer);
  orientationApplied = true;
  renderer.requestNextFullRefresh();

  memset(quotes, 0, sizeof(quotes));
  quoteCount = STOCK_WATCHLIST.getCount();
  for (int i = 0; i < quoteCount; i++) {
    strncpy(quotes[i].symbol, STOCK_WATCHLIST.getSymbol(i), STOCK_SYMBOL_MAX_LEN);
    quotes[i].symbol[STOCK_SYMBOL_MAX_LEN] = '\0';
  }

  // Start from the last known snapshot so the screen is never blank while the
  // N symbol fetch runs.
  if (STOCK_CACHE.hasCache()) {
    const StockQuote* cached = STOCK_CACHE.getQuotes();
    const int cachedCount = STOCK_CACHE.getQuoteCount();
    for (int i = 0; i < quoteCount && i < cachedCount; i++) {
      if (strcmp(cached[i].symbol, quotes[i].symbol) == 0) {
        quotes[i] = cached[i];
        quotes[i].stale = true;
      }
    }
  }

  showingCachedData = STOCK_CACHE.hasCache();
  pageIndex = 0;
  lastFetchMs = 0;
  nextFetchAtMs = 0;
  lastTickMs = 0;

  state = WIFI_CONNECTING;
  statusMessage = tr(STR_FETCHING_STOCKS);
  requestUpdate(true);

  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected();
  } else if (!beginSilentWifiConnect()) {
    if (STOCK_CACHE.hasCache()) {
      state = DISPLAYING;
      requestUpdate(true);
    } else {
      goToWifiSelection();
    }
  }
}

void StockTickerActivity::onExit() {
  // The cached index value stays valid on purpose: the top bar keeps showing
  // the last known value until a newer refresh replaces it.
  SETTINGS.saveToFile();

  if (orientationApplied) {
    renderer.setOrientation(originalOrientation);
    orientationApplied = false;
  }
  Activity::onExit();
}

bool StockTickerActivity::beginSilentWifiConnect() {
  const auto& ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) return false;
  const std::optional<WifiCredential> cred = WIFI_STORE.findCredential(ssid);
  if (!cred) return false;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  wifiConnectStartMs = millis();
  return true;
}

void StockTickerActivity::goToWifiSelection() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& r) {
                           if (r.isCancelled) {
                             finish();
                             return;
                           }
                           onWifiConnected();
                         });
}

void StockTickerActivity::onWifiConnected() {
  NetworkMemory::prepareBeforeNetwork(renderer, "STOCK", "before-quotes");
  // TLS certificate validation needs a plausible clock.
  TimeUtils::syncTimeWithNtp(5000);
  startFetch();
}

// --- refresh loop ----------------------------------------------------------

void StockTickerActivity::startFetch() {
  quoteCount = STOCK_WATCHLIST.getCount();
  for (int i = 0; i < quoteCount; i++) {
    strncpy(quotes[i].symbol, STOCK_WATCHLIST.getSymbol(i), STOCK_SYMBOL_MAX_LEN);
    quotes[i].symbol[STOCK_SYMBOL_MAX_LEN] = '\0';
  }
  fetchIndex = 0;
  lastFetchMs = millis();
  lastTickMs = lastFetchMs;
  state = FETCHING;
  statusMessage = tr(STR_FETCHING_STOCKS);
  requestUpdate(true);
}

void StockTickerActivity::tickFetch() {
  if (fetchIndex >= quoteCount) {
    finishFetch();
    return;
  }

  const uint32_t now = millis();
  // One TLS handshake per loop() iteration keeps the main task responsive and
  // the watchdog fed; a blocking loop over N symbols is what makes the weather
  // app feel frozen on a C3.
  if (now - lastTickMs < MAX_TICK_GAP_MS) return;
  lastTickMs = now;

  const char* symbol = quotes[fetchIndex].symbol;
  char encoded[STOCK_SYMBOL_MAX_LEN * 3 + 1];
  if (!StockFormat::stockUrlEncodeSymbol(symbol, encoded, sizeof(encoded))) {
    fetchIndex++;
    return;
  }

  char url[160];
  bool fetched = false;
  if (esp_get_free_heap_size() >= HttpDownloader::MIN_TLS_FREE_HEAP) {
    std::string response;
    StockQuote parsed;
    // Try query1, then query2, before declaring the symbol failed. Each attempt
    // is its own TLS handshake, but this only runs on the (rare) error path.
    const char* const hosts[] = {YAHOO_HOST, YAHOO_FALLBACK_HOST};
    for (const char* host : hosts) {
      snprintf(url, sizeof(url), "https://%s%s%s%s", host, YAHOO_PATH, encoded, YAHOO_QUERY);
      if (!HttpDownloader::fetchUrl(url, response)) {
        LOG_ERR("STOCK", "Fetch failed for %s via %s", symbol, host);
        continue;
      }
      if (!StockQuoteParser::parse(response, symbol, parsed)) {
        LOG_ERR("STOCK", "Parse failed for %s via %s", symbol, host);
        continue;
      }
      // Keep the previous good row on failure; on success overwrite and
      // clear the stale flag.
      quotes[fetchIndex] = parsed;
      fetched = true;
      break;
    }
  } else {
    LOG_ERR("STOCK", "Skipping %s: free heap %lu below TLS floor", symbol,
             static_cast<unsigned long>(esp_get_free_heap_size()));
  }

  if (!fetched) {
    // Preserve whatever is already on screen for this symbol and flag it.
    if (quotes[fetchIndex].price > 0.0f) {
      quotes[fetchIndex].stale = true;
    } else {
      // Never fetched and no cache: mark invalid so render skips the price.
      quotes[fetchIndex].valid = false;
    }
    // Sticky for the whole sweep: one failed symbol still means the page as a
    // whole is not fully live, so later successes must not clear the flag.
    showingCachedData = true;
  }

  fetchIndex++;
  requestUpdate(true);
}

void StockTickerActivity::finishFetch() {
  NetworkMemory::restoreAfterNetwork(renderer, "STOCK", "after-quotes");

  // Persist whatever is displayable. Rows that failed keep their previous
  // values so the offline view is never emptier than what the user last saw.
  int32_t epoch = static_cast<int32_t>(TimeUtils::getCurrentValidTimestamp());
  char stamp[8] = "";
  if (epoch > 0) {
    const time_t t = static_cast<time_t>(static_cast<int64_t>(epoch));
    struct tm ti;
    localtime_r(&t, &ti);
    snprintf(stamp, sizeof(stamp), "%02d:%02d", ti.tm_hour, ti.tm_min);
  }
  STOCK_CACHE.update(quotes, quoteCount, epoch, stamp);

  refreshTopbarValue();

  lastFetchMs = millis();
  computeAutoRefreshDelay();

  if (!showingCachedData) {
    state = DISPLAYING;
  } else {
    // Partial or full failure: still render, but flag it.
    bool anyLive = false;
    for (int i = 0; i < quoteCount; i++) {
      if (quotes[i].valid && !quotes[i].stale) {
        anyLive = true;
        break;
      }
    }
    if (!anyLive && !STOCK_CACHE.hasCache()) {
      state = FETCH_ERROR;
      statusMessage = tr(STR_STOCK_ERROR);
    } else {
      state = DISPLAYING;
    }
  }
  requestUpdate(true);
}

void StockTickerActivity::refreshTopbarValue() {
  if (!SETTINGS.stockTopbarEnabled) return;
  const StockQuote* chosen = nullptr;
  for (int i = 0; i < quoteCount; i++) {
    if (!quotes[i].valid) continue;
    if (quotes[i].symbol[0] == '^') {
      chosen = &quotes[i];
      break;
    }
  }
  if (chosen == nullptr) {
    SETTINGS.stockTopbarValid = false;
    return;
  }
  SETTINGS.stockTopbarValue = chosen->price;
  SETTINGS.stockTopbarChangePositive = chosen->change >= 0.0f;
  SETTINGS.stockTopbarValid = true;
}

uint32_t StockTickerActivity::effectiveRefreshMs() const {
  switch (SETTINGS.stockRefreshInterval) {
    case CrossPointSettings::STOCK_REFRESH_5M:
      return 5UL * 60UL * 1000UL;
    case CrossPointSettings::STOCK_REFRESH_10M:
      return 10UL * 60UL * 1000UL;
    case CrossPointSettings::STOCK_REFRESH_15M:
      return 15UL * 60UL * 1000UL;
    case CrossPointSettings::STOCK_REFRESH_30M:
      return 30UL * 60UL * 1000UL;
    case CrossPointSettings::STOCK_REFRESH_1H:
      return 60UL * 60UL * 1000UL;
    case CrossPointSettings::STOCK_REFRESH_4H:
      return 4UL * 60UL * 60UL * 1000UL;
    case CrossPointSettings::STOCK_REFRESH_AUTO:
    default:
      break;
  }
  // AUTO: the feed's delay tracks the exchange session, so poll often while
  // something is open and back off hard when everything is shut.
  return marketSummary().primary == STOCK_MARKET_CLOSED ? 60UL * 60UL * 1000UL : 5UL * 60UL * 1000UL;
}

void StockTickerActivity::computeAutoRefreshDelay() {
  nextFetchAtMs = lastFetchMs + effectiveRefreshMs();
}

void StockTickerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      onWifiConnected();
      return;
    }
    if (millis() - wifiConnectStartMs > WIFI_SILENT_CONNECT_TIMEOUT_MS) {
      if (STOCK_CACHE.hasCache()) {
        state = DISPLAYING;
        showingCachedData = true;
        requestUpdate(true);
      } else {
        goToWifiSelection();
      }
    }
    return;
  }

  if (state == FETCHING) {
    tickFetch();
    return;
  }

  // DISPLAYING / FETCH_ERROR: handle input, then honor the auto refresh.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (WiFi.status() == WL_CONNECTED) {
      onWifiConnected();
    } else if (!beginSilentWifiConnect()) {
      goToWifiSelection();
    } else {
      state = WIFI_CONNECTING;
      statusMessage = tr(STR_FETCHING_STOCKS);
      requestUpdate(true);
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (pageIndex + 1 < totalPages) {
      pageIndex++;
      requestUpdate(true);
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (pageIndex > 0) {
      pageIndex--;
      requestUpdate(true);
    }
  }

  if (showingCachedData && lastFetchMs == 0) {
    nextFetchAtMs = 0;
  } else if (nextFetchAtMs != 0 && static_cast<int32_t>(millis() - nextFetchAtMs) >= 0) {
    nextFetchAtMs = 0;
    if (WiFi.status() == WL_CONNECTED) {
      onWifiConnected();
    } else if (beginSilentWifiConnect()) {
      state = WIFI_CONNECTING;
      statusMessage = tr(STR_FETCHING_STOCKS);
      requestUpdate(true);
    } else {
      goToWifiSelection();
    }
  }
}

// --- time/market helpers ---------------------------------------------------

int32_t StockTickerActivity::nowEpoch() const {
  return static_cast<int32_t>(TimeUtils::getCurrentValidTimestamp());
}

StockMarketSummary StockTickerActivity::marketSummary() const {
  return StockFormat::stockComputeSummary(quotes, quoteCount, nowEpoch());
}

const char* StockTickerActivity::marketStateLabel(StockMarketState state) const {
  switch (state) {
    case STOCK_MARKET_OPEN:
      return tr(STR_MARKET_OPEN);
    case STOCK_MARKET_PRE:
      return tr(STR_MARKET_PRE);
    case STOCK_MARKET_POST:
      return tr(STR_MARKET_POST);
    case STOCK_MARKET_CLOSED:
    default:
      return tr(STR_MARKET_CLOSED);
  }
}

void StockTickerActivity::formatExchangeTime(int32_t epoch, char* out, size_t outSize) const {
  // Do NOT swap the process TZ here: TimeUtils::configureTimezone() mutates
  // global state and runs on render paths. gmtime_r with the exchange's own
  // offset is thread-safe and needs no state change.
  if (outSize == 0) return;
  if (epoch <= 0) {
    out[0] = '\0';
    return;
  }
  // Use the first row that actually has an exchange offset, not just row 0:
  // a failed first row leaves gmtoffset at 0 (UTC) and would mislead.
  int32_t offset = 0;
  for (int i = 0; i < quoteCount; i++) {
    if (quotes[i].valid) {
      offset = quotes[i].gmtoffset;
      break;
    }
  }
  const time_t t = static_cast<time_t>(static_cast<int64_t>(epoch) + offset);
  struct tm ti;
  gmtime_r(&t, &ti);
  snprintf(out, outSize, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

void StockTickerActivity::formatLocalTime(int32_t epoch, char* out, size_t outSize) const {
  if (epoch <= 0 || outSize == 0) {
    if (outSize) out[0] = '\0';
    return;
  }
  const time_t t = static_cast<time_t>(static_cast<int64_t>(epoch));
  struct tm ti;
  localtime_r(&t, &ti);
  snprintf(out, outSize, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

// --- pagination ------------------------------------------------------------

void StockTickerActivity::updatePagination() {
  const int priceH = renderer.getLineHeight(STOCK_PRICE_FONT);
  const int changeH = renderer.getLineHeight(STOCK_CHANGE_FONT);
  rowHeight = priceH + changeH + 14;
  if (rowHeight < 40) rowHeight = 40;
}

void StockTickerActivity::drawStatusLine(int y, const Rect& bounds) {
  char buf[96];
  const StockMarketSummary summary = marketSummary();
  const char* stateLabel = marketStateLabel(summary.primary);

  const char* tz = "";
  for (int i = 0; i < quoteCount; i++) {
    if (quotes[i].valid && quotes[i].tzAbbr[0] != '\0') {
      tz = quotes[i].tzAbbr;
      break;
    }
  }

  if (summary.mixedCount > 0) {
    snprintf(buf, sizeof(buf), "%s%s%s · %d/%d %s", stateLabel, (tz[0] != '\0') ? " · " : "", tz,
             summary.mixedCount, summary.totalCount, tr(STR_STOCK_MARKETS_DIFFER));
  } else {
    snprintf(buf, sizeof(buf), "%s%s%s", stateLabel, (tz[0] != '\0') ? " · " : "", tz);
  }

  const int w = renderer.getTextWidth(STOCK_STATUS_FONT, buf);
  renderer.drawText(STOCK_STATUS_FONT, bounds.x + (bounds.width - w) / 2, y, buf);
}

void StockTickerActivity::drawRows(int rowsTop, int rowsBottom, const Rect& bounds) {
  char priceBuf[32];
  char changeBuf[48];
  const int priceH = renderer.getLineHeight(STOCK_PRICE_FONT);
  const int changeH = renderer.getLineHeight(STOCK_CHANGE_FONT);
  const int sidePad = UITheme::getInstance().getMetrics().contentSidePadding;
  const int leftX = bounds.x + sidePad;
  const int rightX = bounds.x + bounds.width - sidePad;

  int y = rowsTop;
  const int first = pageIndex * rowsPerPage;

  for (int i = 0; i < rowsPerPage && first + i < quoteCount; i++) {
    const StockQuote& q = quotes[first + i];
    if (!q.valid) {
      y += rowHeight;
      continue;
    }

    // Symbol on the left, price right-aligned on the same baseline.
    renderer.drawText(STOCK_PRICE_FONT, leftX, y, q.symbol, true, EpdFontFamily::BOLD);

    StockFormat::stockFormatNumber(q.price, 2, priceBuf, sizeof(priceBuf));
    const int priceW = renderer.getTextWidth(STOCK_PRICE_FONT, priceBuf, EpdFontFamily::BOLD);
    renderer.drawText(STOCK_PRICE_FONT, rightX - priceW, y, priceBuf, true, EpdFontFamily::BOLD);

    char signedChange[24];
    StockFormat::stockFormatSigned(q.change, 2, signedChange, sizeof(signedChange));
    snprintf(changeBuf, sizeof(changeBuf), "%s (%s%.*f%%)", signedChange, q.changePercent >= 0.0f ? "+" : "",
             2, static_cast<double>(q.changePercent));

    const int changeW = renderer.getTextWidth(STOCK_CHANGE_FONT, changeBuf);
    const int changeX = rightX - changeW;
    renderer.drawText(STOCK_CHANGE_FONT, changeX, y + priceH - 2, changeBuf);

    const int triCx = changeX - 10 > leftX + 5 ? changeX - 10 : leftX + 5;
    const int triY = y + priceH - 2 + (changeH - 6) / 2;
    if (q.change > 0.0f) {
      drawTriangleUp(renderer, triCx, triY);
    } else if (q.change < 0.0f) {
      drawTriangleDown(renderer, triCx, triY);
    }

    y += rowHeight;
    if (y + rowHeight > rowsBottom) break;
  }
}

void StockTickerActivity::drawFooter(int ruleY, int footerY, const Rect& bounds) {
  const int sidePad = UITheme::getInstance().getMetrics().contentSidePadding;
  const int leftX = bounds.x + sidePad;
  const int rightX = bounds.x + bounds.width - sidePad;

  // Thin separator above the footer.
  renderer.fillRect(leftX, ruleY, rightX - leftX, 3, true);

  const int32_t epoch = nowEpoch();
  char localTime[8] = "";
  char exchangeTime[8] = "";
  formatLocalTime(epoch, localTime, sizeof(localTime));
  formatExchangeTime(epoch, exchangeTime, sizeof(exchangeTime));

  const char* localTz = TimeUtils::getCurrentTimeZoneLabel();
  const char* exchTz = (quoteCount > 0) ? quotes[0].tzAbbr : "";

  char ageBuf[16] = "";
  int32_t refEpoch = epoch;
  for (int i = 0; i < quoteCount; i++) {
    if (quotes[i].valid && quotes[i].quoteTime > 0) {
      refEpoch = quotes[i].quoteTime;
      break;
    }
  }
  if (refEpoch > 0 && epoch > refEpoch) {
    const int32_t delta = epoch - refEpoch;
    if (delta < 60) {
      snprintf(ageBuf, sizeof(ageBuf), "%ds", static_cast<int>(delta));
    } else if (delta < 3600) {
      snprintf(ageBuf, sizeof(ageBuf), "%dm", static_cast<int>(delta / 60));
    } else {
      snprintf(ageBuf, sizeof(ageBuf), "%dh", static_cast<int>(delta / 3600));
    }
  }

  char left[72];
  snprintf(left, sizeof(left), "%s %s · %s %s%s%s", localTime, localTz ? localTz : "", exchangeTime,
           exchTz ? exchTz : "", (ageBuf[0] ? " " : ""), ageBuf);

  int leftMaxWidth = rightX - leftX;
  if (totalPages > 1) {
    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "%s %d/%d", tr(STR_STOCK_PG), pageIndex + 1, totalPages);
    const int pageW = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
    renderer.drawText(SMALL_FONT_ID, rightX - pageW, footerY, pageBuf);
    leftMaxWidth -= pageW + 12;
  }
  if (leftMaxWidth > 0) {
    const std::string clipped = renderer.truncatedText(SMALL_FONT_ID, left, leftMaxWidth);
    renderer.drawText(SMALL_FONT_ID, leftX, footerY, clipped.c_str());
  }
}

void StockTickerActivity::render(RenderLock&&) {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  const Rect bounds = theme.getScreenSafeArea(renderer, true, false);

  renderer.clearScreen();

  const int headerTop = bounds.y + metrics.topPadding;
  const int statusY = headerTop + metrics.headerHeight + 2;
  const int rowsTop = statusY + renderer.getLineHeight(STOCK_STATUS_FONT) + 6;
  const int footerTextH = renderer.getLineHeight(SMALL_FONT_ID);
  const int footerY = bounds.y + bounds.height - footerTextH - 4;
  const int ruleY = footerY - 8;

  if (state == WIFI_CONNECTING || state == FETCHING || state == FETCH_ERROR) {
    GUI.drawHeader(renderer, Rect{bounds.x, headerTop, bounds.width, metrics.headerHeight}, tr(STR_STOCK_TICKER));
    UITheme::drawCenteredText(renderer, bounds, UI_12_FONT_ID, rowsTop, statusMessage.c_str());
    renderer.displayBuffer();
    return;
  }

  GUI.drawHeader(renderer, Rect{bounds.x, headerTop, bounds.width, metrics.headerHeight}, tr(STR_STOCK_TICKER));
  drawStatusLine(statusY, bounds);

  const int rowsBottom = ruleY - 2;
  const int usable = rowsBottom - rowsTop;
  if (usable < rowHeight) {
    renderer.displayBuffer();
    return;
  }

  updatePagination();
  rowsPerPage = std::max(1, usable / rowHeight);
  totalPages = std::max(1, (quoteCount + rowsPerPage - 1) / rowsPerPage);
  if (pageIndex >= totalPages) pageIndex = totalPages - 1;

  drawRows(rowsTop, rowsBottom, bounds);
  drawFooter(ruleY, footerY, bounds);

  const char* prevLabel = totalPages > 1 ? tr(STR_DIR_UP) : "";
  const char* nextLabel = totalPages > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_REFRESH), prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
