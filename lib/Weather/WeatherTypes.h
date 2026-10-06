#pragma once

#include <cstdint>

// Plain weather data shared between WeatherActivity (live fetch/render) and
// WeatherCacheStore (last-known-good snapshot persisted for offline display).
// Kept dependency-free so the parser runs in native unit tests.

constexpr int WEATHER_FORECAST_DAYS = 5;
constexpr int WEATHER_CITY_NAME_LEN = 32;
// Sentinel for "no temperature known yet" in the header top-bar value.
constexpr int8_t WEATHER_TEMP_UNAVAILABLE = -128;

struct WeatherData {
  float temperature = 0;
  float feelsLike = 0;
  int humidity = 0;
  int weatherCode = 0;
  float windSpeed = 0;  // km/h
  float dewPoint = 0;
  float pressure = 0;  // surface pressure (hPa)
  float uvIndex = 0;
  int airQuality = -1;           // US AQI; -1 when unavailable
  char sunrise[6] = "";          // "HH:MM" (city local time)
  char sunset[6] = "";           // "HH:MM" (city local time)
  char observed[6] = "";         // "HH:MM" of the current reading (city local time)
  int32_t utcOffsetSeconds = 0;  // city offset from UTC
};

struct DailyForecast {
  int weatherCode = 0;
  float tempMax = 0;
  float tempMin = 0;
  uint8_t weekday = 0;  // 0 = Sunday .. 6 = Saturday
};

enum class WeatherCondition : uint8_t { Clear, PartlyCloudy, Fog, Drizzle, Rain, Snow, Showers, Thunderstorm };

struct WeatherCity {
  const char* name;
  const char* lat;
  const char* lon;
};
