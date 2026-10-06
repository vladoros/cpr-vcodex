#pragma once

#include <cstddef>
#include <cstdint>

#include "CrossPointSettings.h"

// Refresh choices for the network apps, stored in SETTINGS as table indices.
// Values are minutes; 0 in the stock table means Auto (5 min while a market is
// open, hourly when every market is closed).
namespace AppRefreshInterval {

inline constexpr uint16_t kWeatherMinutes[] = {5, 15, 30, 60};
inline constexpr uint16_t kStockMinutes[] = {0, 5, 10, 15, 30, 60, 240};
inline constexpr uint16_t kStockAutoOpenMinutes = 5;
inline constexpr uint16_t kStockAutoClosedMinutes = 60;

static_assert(sizeof(kWeatherMinutes) / sizeof(kWeatherMinutes[0]) == CrossPointSettings::APP_WEATHER_INTERVAL_CHOICES);
static_assert(sizeof(kStockMinutes) / sizeof(kStockMinutes[0]) == CrossPointSettings::APP_STOCK_INTERVAL_CHOICES);

template <size_t N>
uint16_t minutesAt(const uint16_t (&table)[N], const uint8_t index) {
  return table[index < N ? index : 0];
}

inline uint32_t minutesToMs(const uint16_t minutes) { return static_cast<uint32_t>(minutes) * 60UL * 1000UL; }

}  // namespace AppRefreshInterval
