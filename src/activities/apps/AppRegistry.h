#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

#include "AppCapabilities.h"
#include "components/themes/BaseTheme.h"

enum class AppId : uint8_t { ClaudeBuddy, Weather, StockTicker };

struct AppDefinition {
  AppId id;
  StrId name;
  StrId desc;
  UIIcon icon;
};

#if CROSSINK_APP_HAS_APPS
inline constexpr AppDefinition kApps[] = {
#if CROSSINK_APP_CAP_CLAUDE_BUDDY
    {AppId::ClaudeBuddy, StrId::STR_CLAUDE_BUDDY, StrId::STR_CLAUDE_BUDDY_DESC, UIIcon::Bluetooth},
#endif
#if CROSSINK_APP_CAP_WEATHER
    {AppId::Weather, StrId::STR_WEATHER, StrId::STR_WEATHER_DESC, UIIcon::Weather},
#endif
#if CROSSINK_APP_CAP_STOCKS
    {AppId::StockTicker, StrId::STR_STOCK_TICKER, StrId::STR_STOCK_TICKER_DESC, UIIcon::Stocks},
#endif
};
inline constexpr size_t kAppCount = sizeof(kApps) / sizeof(kApps[0]);
#else
inline constexpr size_t kAppCount = 0;
#endif
