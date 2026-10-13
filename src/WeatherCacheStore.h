#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include "WeatherTypes.h"

/**
 * @brief Singleton store for the last successfully fetched weather snapshot.
 *
 * WeatherActivity writes here after every successful fetch and reads from
 * here to render a "last known" view when WiFi/the weather API is
 * unavailable, instead of forcing the user through the WiFi picker or
 * showing a bare error.
 */
class WeatherCacheStore : public PersistableStore<WeatherCacheStore> {
 private:
  WeatherData weather;
  DailyForecast forecast[WEATHER_FORECAST_DAYS];
  int forecastCount = 0;
  char cityName[32] = "";
  char lastUpdateTime[8] = "";
  bool cached = false;

  WeatherCacheStore() = default;

  friend class PersistableStore<WeatherCacheStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/weather_cache.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Persists a fresh snapshot after a successful fetch.
  bool update(const WeatherData& freshWeather, const DailyForecast* freshForecast, int freshForecastCount,
              const char* freshCityName, const char* freshLastUpdateTime);

  bool hasCache() const { return cached; }
  const WeatherData& getWeather() const { return weather; }
  const DailyForecast* getForecast() const { return forecast; }
  int getForecastCount() const { return forecastCount; }
  const char* getCityName() const { return cityName; }
  const char* getLastUpdateTime() const { return lastUpdateTime; }
};

#define WEATHER_CACHE WeatherCacheStore::getInstance()
