#include "WeatherParser.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace WeatherParser {

const WeatherCity kCities[kCityCount] = {
    {"Madrid", "40.4168", "-3.7038"},         {"London", "51.5074", "-0.1278"},
    {"Paris", "48.8566", "2.3522"},           {"Berlin", "52.5200", "13.4050"},
    {"Rome", "41.9028", "12.4964"},           {"Athens", "37.9838", "23.7275"},
    {"Helsinki", "60.1699", "24.9384"},       {"Moscow", "55.7558", "37.6173"},
    {"New York", "40.7128", "-74.0060"},      {"Chicago", "41.8781", "-87.6298"},
    {"Denver", "39.7392", "-104.9903"},       {"Los Angeles", "34.0522", "-118.2437"},
    {"Mexico City", "19.4326", "-99.1332"},   {"Sao Paulo", "-23.5505", "-46.6333"},
    {"Buenos Aires", "-34.6037", "-58.3816"}, {"Johannesburg", "-26.2041", "28.0473"},
    {"Nairobi", "-1.2921", "36.8219"},        {"Dubai", "25.2048", "55.2708"},
    {"Karachi", "24.8607", "67.0011"},        {"New Delhi", "28.6139", "77.2090"},
    {"Dhaka", "23.8103", "90.4125"},          {"Bangkok", "13.7563", "100.5018"},
    {"Singapore", "1.3521", "103.8198"},      {"Beijing", "39.9042", "116.4074"},
    {"Tokyo", "35.6762", "139.6503"},         {"Seoul", "37.5665", "126.9780"},
    {"Sydney", "-33.8688", "151.2093"},       {"Auckland", "-36.8485", "174.7633"},
};

namespace {
void copyStr(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (src == nullptr) src = "";
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// Copies "HH:MM" out of an ISO local timestamp "YYYY-MM-DDTHH:MM".
void copyIsoTime(char (&dst)[6], const char* iso) {
  dst[0] = '\0';
  if (iso == nullptr || strlen(iso) < 16 || iso[10] != 'T') return;
  memcpy(dst, iso + 11, 5);
  dst[5] = '\0';
}
}  // namespace

WeatherCondition conditionForCode(const int code) {
  if (code <= 0) return WeatherCondition::Clear;
  if (code <= 3) return WeatherCondition::PartlyCloudy;
  if (code <= 48) return WeatherCondition::Fog;
  if (code <= 57) return WeatherCondition::Drizzle;
  if (code <= 67) return WeatherCondition::Rain;
  if (code <= 77) return WeatherCondition::Snow;
  if (code <= 82) return WeatherCondition::Showers;
  if (code <= 86) return WeatherCondition::Snow;  // snow showers
  return WeatherCondition::Thunderstorm;
}

bool weekdayFromIsoDate(const char* date, uint8_t& weekday) {
  int y = 0;
  int m = 0;
  int d = 0;
  if (date == nullptr || sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) return false;
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;
  // Sakamoto's method: pure arithmetic, so no mktime()/TZ dependency.
  static const int kMonthOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  weekday = static_cast<uint8_t>((y + y / 4 - y / 100 + y / 400 + kMonthOffsets[m - 1] + d) % 7);
  return true;
}

bool parseForecast(const char* json, const size_t length, WeatherData& weather, DailyForecast* forecast,
                   int& forecastCount) {
  forecastCount = 0;
  if (json == nullptr || length == 0) return false;

  JsonDocument filter;
  filter["utc_offset_seconds"] = true;
  filter["current"] = true;
  filter["daily"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, length, DeserializationOption::Filter(filter))) return false;

  JsonObjectConst current = doc["current"].as<JsonObjectConst>();
  if (current.isNull() || !current["temperature_2m"].is<float>()) return false;

  weather.temperature = current["temperature_2m"] | 0.0f;
  weather.feelsLike = current["apparent_temperature"] | 0.0f;
  weather.humidity = current["relative_humidity_2m"] | 0;
  weather.weatherCode = current["weather_code"] | 0;
  weather.windSpeed = current["wind_speed_10m"] | 0.0f;
  weather.dewPoint = current["dew_point_2m"] | 0.0f;
  weather.pressure = current["surface_pressure"] | 0.0f;
  weather.uvIndex = current["uv_index"] | 0.0f;
  weather.utcOffsetSeconds = doc["utc_offset_seconds"] | static_cast<int32_t>(0);
  copyIsoTime(weather.observed, current["time"] | "");
  weather.sunrise[0] = '\0';
  weather.sunset[0] = '\0';

  JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
  if (daily.isNull()) return true;

  copyIsoTime(weather.sunrise, daily["sunrise"][0] | "");
  copyIsoTime(weather.sunset, daily["sunset"][0] | "");

  JsonArrayConst codes = daily["weather_code"].as<JsonArrayConst>();
  JsonArrayConst maxTemps = daily["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst minTemps = daily["temperature_2m_min"].as<JsonArrayConst>();
  JsonArrayConst dates = daily["time"].as<JsonArrayConst>();
  const int days = static_cast<int>(codes.size());
  // Day 0 is today, already shown as the current reading.
  for (int i = 1; i < days && forecastCount < WEATHER_FORECAST_DAYS; i++) {
    DailyForecast& f = forecast[forecastCount];
    f.weatherCode = codes[i] | 0;
    f.tempMax = maxTemps[i] | 0.0f;
    f.tempMin = minTemps[i] | 0.0f;
    if (!weekdayFromIsoDate(dates[i] | "", f.weekday)) f.weekday = 0;
    forecastCount++;
  }
  return true;
}

bool parseAirQuality(const char* json, const size_t length, int& usAqi) {
  if (json == nullptr || length == 0) return false;
  JsonDocument filter;
  filter["current"]["us_aqi"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, length, DeserializationOption::Filter(filter))) return false;
  JsonVariantConst aqi = doc["current"]["us_aqi"];
  if (!aqi.is<int>() || aqi.as<int>() < 0) return false;
  usAqi = aqi.as<int>();
  return true;
}

bool parseIpLocation(const char* json, const size_t length, char* lat, const size_t latSize, char* lon,
                     const size_t lonSize, char* city, const size_t citySize) {
  if (json == nullptr || length == 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json, length)) return false;
  if (!doc["lat"].is<float>() || !doc["lon"].is<float>()) return false;
  const float latValue = doc["lat"].as<float>();
  const float lonValue = doc["lon"].as<float>();
  if (latValue == 0.0f && lonValue == 0.0f) return false;
  snprintf(lat, latSize, "%.4f", static_cast<double>(latValue));
  snprintf(lon, lonSize, "%.4f", static_cast<double>(lonValue));
  copyStr(city, citySize, doc["city"] | "");
  return true;
}

}  // namespace WeatherParser
