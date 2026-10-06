#include "WeatherCacheStore.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace {
template <size_t N>
void copyStr(char (&dst)[N], const char* src) {
  strncpy(dst, src ? src : "", N - 1);
  dst[N - 1] = '\0';
}
}  // namespace

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
  doc["observed"] = weather.observed;
  doc["utcOffset"] = weather.utcOffsetSeconds;
  doc["cityName"] = cityName;
  doc["updatedAt"] = updatedAt;

  JsonArray arr = doc["forecast"].to<JsonArray>();
  for (int i = 0; i < forecastCount; i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["code"] = forecast[i].weatherCode;
    obj["max"] = forecast[i].tempMax;
    obj["min"] = forecast[i].tempMin;
    obj["weekday"] = forecast[i].weekday;
  }
}

bool WeatherCacheStore::fromJson(JsonVariantConst doc) {
  if (!doc["temperature"].is<float>()) return false;
  weather.temperature = doc["temperature"] | 0.0f;
  weather.feelsLike = doc["feelsLike"] | 0.0f;
  weather.humidity = doc["humidity"] | 0;
  weather.weatherCode = doc["weatherCode"] | 0;
  weather.windSpeed = doc["windSpeed"] | 0.0f;
  weather.dewPoint = doc["dewPoint"] | 0.0f;
  weather.pressure = doc["pressure"] | 0.0f;
  weather.uvIndex = doc["uvIndex"] | 0.0f;
  weather.airQuality = doc["airQuality"] | -1;
  copyStr(weather.sunrise, doc["sunrise"] | "");
  copyStr(weather.sunset, doc["sunset"] | "");
  copyStr(weather.observed, doc["observed"] | "");
  weather.utcOffsetSeconds = doc["utcOffset"] | static_cast<int32_t>(0);
  copyStr(cityName, doc["cityName"] | "");
  copyStr(updatedAt, doc["updatedAt"] | "");

  forecastCount = 0;
  for (JsonObjectConst obj : doc["forecast"].as<JsonArrayConst>()) {
    if (forecastCount >= WEATHER_FORECAST_DAYS) break;
    DailyForecast& f = forecast[forecastCount++];
    f.weatherCode = obj["code"] | 0;
    f.tempMax = obj["max"] | 0.0f;
    f.tempMin = obj["min"] | 0.0f;
    f.weekday = static_cast<uint8_t>((obj["weekday"] | 0) % 7);
  }

  cached = true;
  LOG_DBG("WEATHER", "Loaded cached weather for %s (%s)", cityName, updatedAt);
  return true;
}

bool WeatherCacheStore::update(const WeatherData& freshWeather, const DailyForecast* freshForecast,
                               const int freshForecastCount, const char* freshCityName, const char* freshUpdatedAt) {
  weather = freshWeather;
  forecastCount = std::clamp(freshForecastCount, 0, WEATHER_FORECAST_DAYS);
  for (int i = 0; i < forecastCount; i++) forecast[i] = freshForecast[i];
  copyStr(cityName, freshCityName);
  copyStr(updatedAt, freshUpdatedAt);
  cached = true;
  return saveToFile();
}
