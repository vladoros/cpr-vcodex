#pragma once

#include <cstddef>

#include "WeatherTypes.h"

namespace WeatherParser {

constexpr int kCityCount = 28;
// Manual city choices. Index 0 of the weatherCity setting means "Auto" (IP
// geolocation); setting value N >= 1 selects kCities[N - 1].
extern const WeatherCity kCities[kCityCount];

// Maps a WMO weather interpretation code (Open-Meteo weather_code) to the
// coarse condition the UI has a label and icon for.
WeatherCondition conditionForCode(int wmoCode);

// Day of week (0 = Sunday) for a "YYYY-MM-DD" date, without touching the
// process timezone. Returns false for a malformed date.
bool weekdayFromIsoDate(const char* date, uint8_t& weekday);

// Open-Meteo /v1/forecast with current + daily blocks and timezone=auto.
// Day 0 (today) feeds sunrise/sunset; the next WEATHER_FORECAST_DAYS days
// fill forecast. airQuality is left untouched.
bool parseForecast(const char* json, size_t length, WeatherData& weather, DailyForecast* forecast, int& forecastCount);

// Open-Meteo air-quality /v1/air-quality?current=us_aqi.
bool parseAirQuality(const char* json, size_t length, int& usAqi);

// ip-api.com /json/?fields=lat,lon,city. lat/lon are written as "%.4f".
bool parseIpLocation(const char* json, size_t length, char* lat, size_t latSize, char* lon, size_t lonSize, char* city,
                     size_t citySize);

}  // namespace WeatherParser
