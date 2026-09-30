#pragma once

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class StockTickerInfoActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::vector<std::string> lines;
  int scrollOffset = 0;

  void loadContent();
  int getVisibleLineCount() const;
  int getMaxScrollOffset() const;
  void appendParagraph(const std::string& text, int textWidth);

 public:
  explicit StockTickerInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("StockTickerInfo", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
