#pragma once

#include <GfxRenderer.h>

#include "fontIds.h"

#if CROSSINK_SCALABLE_FONTS
#include <ScalableBuiltins.h>
#endif

// Large display text (temperatures, prices) for the network apps. Scalable
// builds rasterize the built-in Lexend Deca outline at the requested size; the
// C3 bitmap build tops out at its 16 pt Lexend Deca face.
inline int appDisplayFontId(GfxRenderer& renderer, const unsigned points) {
#if CROSSINK_SCALABLE_FONTS
  ensureScalableBuiltinFamily(renderer, 0);
  return scalableBuiltinReaderFontId(0, points);
#else
  (void)renderer;
  if (points >= 16) return LEXENDDECA_16_FONT_ID;
  if (points >= 14) return LEXENDDECA_14_FONT_ID;
  if (points >= 12) return LEXENDDECA_12_FONT_ID;
  return LEXENDDECA_10_FONT_ID;
#endif
}
