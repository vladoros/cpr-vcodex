#pragma once

#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_STOCKS

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Scrollable help text: data source, watchlist location and JSON format.
class StockTickerInfoActivity final : public Activity {
 public:
  StockTickerInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("StockTickerInfo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator_;
  std::vector<std::string> lines_;
  int scrollOffset_ = 0;
  int visibleLines_ = 1;

  void buildLines(int textWidth);
  void scrollBy(int delta);
};

#endif
