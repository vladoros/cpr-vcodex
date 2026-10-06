#pragma once

#include <GfxRenderer.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"

// Header and content frame for the network apps. Both are clipped to the
// screen safe area so the button hints never overlap them, whichever side
// they sit on in the app's chosen orientation.
namespace AppChrome {

inline Rect headerRect(const GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return Rect{safe.x, safe.y + metrics.topPadding, safe.width, TouchHeaderBackButton::height(metrics, input)};
}

// Draws the header and returns the content area below it.
inline Rect drawHeader(GfxRenderer& renderer, const MappedInputManager& input, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = headerRect(renderer, input);
  if (input.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int top = header.y + header.height + metrics.verticalSpacing;
  const int bottom = safe.y + safe.height - metrics.verticalSpacing;
  return Rect{safe.x + metrics.contentSidePadding, top, std::max(1, safe.width - metrics.contentSidePadding * 2),
              std::max(1, bottom - top)};
}

inline bool backTapped(const MappedInputManager& input, const GfxRenderer& renderer) {
  return TouchHeaderBackButton::wasTapped(input, headerRect(renderer, input));
}

}  // namespace AppChrome
