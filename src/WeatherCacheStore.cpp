#include "WeatherCacheStore.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

void WeatherCacheStore::toJson(JsonDocument& doc) const {
  doc["temperature"] = weather.temperature;
  doc["feelsLike"] = weather.feelsLike;
  doc["humidity"] = weather.humidity;
  doc["weatherCode"] = weather.weatherCode;
  doc["windSpeed"] = weather.windSpeed;
  doc["dewPoint"] = weather.dewPoint;
  doc["pressure"] = weather.pressure;
  doc["uvIndex"] = weather.uvIndex;
  doc["airQuality"] = weather.airQuality;
  doc["sunrise"] = weather.sunrise;
  doc["sunset"] = weather.sunset;
  doc["cityName"] = cityName;
  doc["lastUpdateTime"] = lastUpdateTime;

  JsonArray arr = doc["forecast"].to<JsonArray>();
  for (int i = 0; i < forecastCount; i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["code"] = forecast[i].weatherCode;
    obj["max"] = forecast[i].tempMax;
    obj["min"] = forecast[i].tempMin;
    obj["day"] = forecast[i].dayLabel;
  }
}

bool WeatherCacheStore::fromJson(JsonVariantConst doc) {
  weather.temperature = doc["temperature"] | 0.0f;
  weather.feelsLike = doc["feelsLike"] | 0.0f;
  weather.humidity = doc["humidity"] | 0;
  weather.weatherCode = doc["weatherCode"] | 0;
  weather.windSpeed = doc["windSpeed"] | 0.0f;
  weather.dewPoint = doc["dewPoint"] | 0.0f;
  weather.pressure = doc["pressure"] | 0.0f;
  weather.uvIndex = doc["uvIndex"] | 0.0f;
  weather.airQuality = doc["airQuality"] | -1;
  strncpy(weather.sunrise, doc["sunrise"] | "", sizeof(weather.sunrise) - 1);
  weather.sunrise[sizeof(weather.sunrise) - 1] = '\0';
  strncpy(weather.sunset, doc["sunset"] | "", sizeof(weather.sunset) - 1);
  weather.sunset[sizeof(weather.sunset) - 1] = '\0';

  strncpy(cityName, doc["cityName"] | "", sizeof(cityName) - 1);
  cityName[sizeof(cityName) - 1] = '\0';
  strncpy(lastUpdateTime, doc["lastUpdateTime"] | "", sizeof(lastUpdateTime) - 1);
  lastUpdateTime[sizeof(lastUpdateTime) - 1] = '\0';

  forecastCount = 0;
  JsonArrayConst arr = doc["forecast"].as<JsonArrayConst>();
  for (JsonObjectConst obj : arr) {
    if (forecastCount >= WEATHER_FORECAST_DAYS) break;
    auto& f = forecast[forecastCount];
    f.weatherCode = obj["code"] | 0;
    f.tempMax = obj["max"] | 0.0f;
    f.tempMin = obj["min"] | 0.0f;
    strncpy(f.dayLabel, obj["day"] | "", sizeof(f.dayLabel) - 1);
    f.dayLabel[sizeof(f.dayLabel) - 1] = '\0';
    forecastCount++;
  }

  cached = true;
  LOG_DBG("WEATHERCACHE", "Loaded cached weather for %s (%s)", cityName, lastUpdateTime);
  return true;
}

bool WeatherCacheStore::update(const WeatherData& freshWeather, const DailyForecast* freshForecast,
                               int freshForecastCount, const char* freshCityName,
                               const char* freshLastUpdateTime) {
  weather = freshWeather;
  forecastCount = std::min(freshForecastCount, WEATHER_FORECAST_DAYS);
  for (int i = 0; i < forecastCount; i++) forecast[i] = freshForecast[i];

  strncpy(cityName, freshCityName, sizeof(cityName) - 1);
  cityName[sizeof(cityName) - 1] = '\0';
  strncpy(lastUpdateTime, freshLastUpdateTime, sizeof(lastUpdateTime) - 1);
  lastUpdateTime[sizeof(lastUpdateTime) - 1] = '\0';

  cached = true;
  return saveToFile();
}
