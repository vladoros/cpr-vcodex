#pragma once

// Plain weather data shared between WeatherActivity (live fetch/render) and
// WeatherCacheStore (last-known-good snapshot persisted for offline display).
// Kept dependency-free so the store doesn't have to pull in the activity.

constexpr int WEATHER_FORECAST_DAYS = 5;

struct WeatherData {
  float temperature = 0;
  float feelsLike = 0;
  int humidity = 0;
  int weatherCode = 0;
  float windSpeed = 0;
  float dewPoint = 0;
  float pressure = 0;   // surface pressure (hPa)
  float uvIndex = 0;
  int airQuality = -1;  // US AQI; -1 when unavailable
  char sunrise[6] = "";  // "HH:MM" (city local time)
  char sunset[6] = "";   // "HH:MM" (city local time)
};

struct DailyForecast {
  int weatherCode = 0;
  float tempMax = 0;
  float tempMin = 0;
  char dayLabel[4] = "";  // "Mon", "Tue", etc.
};
