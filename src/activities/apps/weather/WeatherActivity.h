#pragma once

#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_WEATHER

#include <WeatherTypes.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/OptionPopup.h"

// Current conditions and a 5-day forecast from Open-Meteo (plain HTTP, no key).
// The last good snapshot is cached on SD and shown whenever Wi-Fi or the API
// is unavailable.
class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == State::Connecting || state_ == State::Fetching; }

 private:
  enum class State : uint8_t { Connecting, Fetching, Displaying, Error };

  State state_ = State::Connecting;
  WeatherData weather_;
  DailyForecast forecast_[WEATHER_FORECAST_DAYS];
  int forecastCount_ = 0;
  char cityName_[WEATHER_CITY_NAME_LEN] = "";
  char updatedAt_[6] = "";
  bool hasData_ = false;
  bool showingCache_ = false;

  // IP geolocation result, kept for the session so Auto costs one lookup.
  char detectedLat_[16] = "";
  char detectedLon_[16] = "";
  char detectedCity_[WEATHER_CITY_NAME_LEN] = "";

  // One reusable response buffer: fetches clear() it, which keeps capacity.
  std::string response_;

  uint32_t connectStartMs_ = 0;
  uint32_t lastFetchMs_ = 0;
  bool userRequestedFetch_ = false;
  bool wifiOwned_ = false;
  bool topbarDirty_ = false;
  GfxRenderer::Orientation originalOrientation_ = GfxRenderer::Portrait;
  int displayFontId_ = 0;

  OptionPopup cityPopup_;
  ScreenTransitionRefresh screenTransitionRefresh_;
  uint8_t fastRefreshCount_ = 0;
  bool forceHalfRefresh_ = false;

  void applyOrientation();
  void startRefresh(bool userInitiated);
  void openWifiSelection();
  void onConnectFailed(bool userInitiated);
  void fetchNow();
  bool resolveCoordinates(const char*& lat, const char*& lon, const char*& city);
  void loadCache();
  void showCacheOrError();
  void openCityPicker();
  void openOptions();
  void exitApp();

  void renderStatus(const Rect& area, const char* text) const;
  void renderPortrait(const Rect& area);
  void renderLandscape(const Rect& area);
  void drawStat(int x, int y, int width, const char* label, const char* value) const;
  void drawForecast(int x, int y, int width) const;
  void drawUpdatedLine(const Rect& area, int y) const;
};

#endif
