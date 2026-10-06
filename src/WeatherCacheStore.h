#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>
#include <WeatherTypes.h>

/**
 * Last successfully fetched weather snapshot, shown when Wi-Fi or the weather
 * API is unavailable instead of an error screen. Disposable firmware data.
 */
class WeatherCacheStore : public PersistableStore<WeatherCacheStore> {
 private:
  WeatherData weather;
  DailyForecast forecast[WEATHER_FORECAST_DAYS];
  int forecastCount = 0;
  char cityName[WEATHER_CITY_NAME_LEN] = "";
  char updatedAt[6] = "";
  bool cached = false;

  WeatherCacheStore() = default;

  friend class PersistableStore<WeatherCacheStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/weather_cache.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Persists a fresh snapshot after a successful fetch.
  bool update(const WeatherData& freshWeather, const DailyForecast* freshForecast, int freshForecastCount,
              const char* freshCityName, const char* freshUpdatedAt);

  bool hasCache() const { return cached; }
  const WeatherData& getWeather() const { return weather; }
  const DailyForecast* getForecast() const { return forecast; }
  int getForecastCount() const { return forecastCount; }
  const char* getCityName() const { return cityName; }
  const char* getUpdatedAt() const { return updatedAt; }
};

#define WEATHER_CACHE WeatherCacheStore::getInstance()
