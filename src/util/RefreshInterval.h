#pragma once

#include "CrossPointSettings.h"

// Converts CrossPointSettings::REFRESH_INTERVAL (shared by WebDash and
// Weather) into milliseconds. 0=30s 1=1m 2=5m (default) 3=15m 4=30m
inline unsigned long refreshIntervalMs(uint8_t interval) {
  switch (interval) {
    case CrossPointSettings::REFRESH_30S:
      return 30UL * 1000;
    case CrossPointSettings::REFRESH_1M:
      return 60UL * 1000;
    case CrossPointSettings::REFRESH_15M:
      return 15UL * 60 * 1000;
    case CrossPointSettings::REFRESH_30M:
      return 30UL * 60 * 1000;
    case CrossPointSettings::REFRESH_5M:
    default:
      return 5UL * 60 * 1000;
  }
}
