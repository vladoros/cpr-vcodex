#pragma once

#include <GfxRenderer.h>

#include "components/UITheme.h"

inline int pageDotsY(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
}

inline void drawPageDots(const GfxRenderer& renderer, const int currentPage, const int totalPages) {
  if (totalPages <= 1) return;
  constexpr int kDotSize = 8;
  constexpr int kDotSpacing = 6;
  const int totalWidth = totalPages * kDotSize + (totalPages - 1) * kDotSpacing;
  const int startX = (renderer.getScreenWidth() - totalWidth) / 2;
  const int y = pageDotsY(renderer);
  for (int page = 0; page < totalPages; ++page) {
    const int x = startX + page * (kDotSize + kDotSpacing);
    if (page == currentPage) {
      renderer.fillRect(x, y, kDotSize, kDotSize, true);
    } else {
      renderer.drawRect(x, y, kDotSize, kDotSize, true);
    }
  }
}
