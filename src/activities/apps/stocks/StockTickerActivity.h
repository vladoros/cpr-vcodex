#pragma once

#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_STOCKS

#include <StockTypes.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

struct Rect;

// Watchlist quotes from Yahoo Finance's chart API, one HTTPS request per symbol
// per loop() tick so Back stays responsive during a sweep. The last good
// quotes are cached on SD and shown stale when a fetch fails.
class StockTickerActivity final : public Activity {
 public:
  StockTickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("StockTicker", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == State::Connecting || state_ == State::Fetching; }

 private:
  enum class State : uint8_t { Connecting, Fetching, Displaying, Error };

  State state_ = State::Connecting;
  StockQuote quotes_[STOCK_MAX_SYMBOLS];
  int quoteCount_ = 0;
  int fetchIndex_ = 0;
  bool sweepHadFailure_ = false;
  bool showingCache_ = false;
  bool topbarDirty_ = false;
  int32_t cacheFetchTime_ = 0;  // epoch of the cached quotes shown as stale

  // One reusable response buffer: fetches clear() it, which keeps capacity.
  std::string response_;

  uint32_t connectStartMs_ = 0;
  uint32_t lastFetchMs_ = 0;
  bool userRequestedFetch_ = false;
  bool wifiOwned_ = false;
  GfxRenderer::Orientation originalOrientation_ = GfxRenderer::Portrait;
  int priceFontId_ = 0;
  int changeFontId_ = 0;

  // Written by render() from the current layout, read by loop() for paging.
  std::atomic<int> rowsPerPage_{1};
  int pageIndex_ = 0;

  ScreenTransitionRefresh screenTransitionRefresh_;
  uint8_t fastRefreshCount_ = 0;
  bool forceHalfRefresh_ = false;

  void applyOrientation();
  void loadWatchlist();
  void startRefresh(bool userInitiated);
  void onConnectFailed(bool userInitiated);
  void openWifiSelection();
  void beginSweep();
  void fetchNextSymbol();
  void finishSweep();
  void updateTopbarValue();
  void openOptions();
  void changePage(int delta);
  int totalPages() const;
  uint32_t refreshIntervalMs() const;
  void exitApp();

  void drawStatusLine(const Rect& area, int y) const;
  void drawRows(const Rect& area, int top, int rowHeight, int rows) const;
  void drawFooter(const Rect& area, int y) const;
};

#endif
