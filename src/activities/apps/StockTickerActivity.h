#pragma once

#include "../Activity.h"

#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "StockTypes.h"

struct Rect;

class StockTickerActivity final : public Activity {
 public:
  enum State { WIFI_CONNECTING, FETCHING, DISPLAYING, FETCH_ERROR };

  explicit StockTickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("StockTicker", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == WIFI_CONNECTING || state == FETCHING; }

 private:
  static constexpr int MAX_SYMBOLS = STOCK_MAX_SYMBOLS;
  static constexpr int MAX_TICK_GAP_MS = 120;
  static constexpr unsigned long WIFI_SILENT_CONNECT_TIMEOUT_MS = 10000;

  State state = WIFI_CONNECTING;
  StockQuote quotes[MAX_SYMBOLS];
  int quoteCount = 0;
  int fetchIndex = 0;

  std::string statusMessage;
  uint32_t lastFetchMs = 0;      // millis() when the current/next fetch started
  uint32_t nextFetchAtMs = 0;    // millis() deadline for the next auto refresh
  uint32_t wifiConnectStartMs = 0;
  uint32_t lastTickMs = 0;

  bool showingCachedData = false;
  bool orientationApplied = false;
  GfxRenderer::Orientation originalOrientation = GfxRenderer::Portrait;

  int pageIndex = 0;
  int rowsPerPage = 1;
  int totalPages = 1;
  int rowHeight = 58;

  void applyStockOrientation();
  bool beginSilentWifiConnect();
  void goToWifiSelection();
  void onWifiConnected();
  void startFetch();
  void tickFetch();
  void finishFetch();
  void computeAutoRefreshDelay();
  void refreshTopbarValue();

  int32_t nowEpoch() const;
  StockMarketSummary marketSummary() const;
  uint32_t effectiveRefreshMs() const;
  const char* marketStateLabel(StockMarketState state) const;
  void updatePagination();
  void drawStatusLine(int y, const Rect& bounds);
  void drawRows(int rowsTop, int rowsBottom, const Rect& bounds);
  void drawFooter(int ruleY, int footerY, const Rect& bounds);
  void formatExchangeTime(int32_t epoch, char* out, size_t outSize) const;
  void formatLocalTime(int32_t epoch, char* out, size_t outSize) const;
};
