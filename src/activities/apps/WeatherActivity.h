#pragma once

#include "../Activity.h"

#include <string>

#include "CrossPointSettings.h"

struct CityCoord {
  const char* name;
  const char* lat;
  const char* lon;
};

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

class WeatherActivity final : public Activity {
 public:
  enum State { WIFI_CONNECTING, FETCHING, DISPLAYING, FETCH_ERROR, SELECTING_CITY };

  // selectedCity index: 0 = Auto (IP geolocation), 1..CITY_COUNT = manual cities
  static constexpr int CITY_COUNT = CrossPointSettings::WEATHER_CITY_LAST;
  static const CityCoord CITIES[CITY_COUNT];

  explicit WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

  static const char* weatherCodeToString(int code);

  // cpr-vcodex has no user-configurable temperature unit; always metric (°C)
  static float convertTemp(float celsius) { return celsius; }
  static const char* tempUnitSuffix() { return "°C"; }

 private:
  static constexpr int FORECAST_DAYS = 5;
  State state = WIFI_CONNECTING;
  WeatherData weather;
  DailyForecast forecast[FORECAST_DAYS];
  int forecastCount = 0;
  uint8_t selectedCity = 0;  // 0 = Auto, 1..CITY_COUNT = manual
  std::string statusMessage;
  char lastUpdateTime[8] = "";
  uint32_t lastFetchMs = 0;  // millis() when the last fetch started, for auto-refresh
  uint32_t wifiConnectStartMs = 0;  // millis() when the silent WiFi connect attempt began
  char detectedCityName[32] = "";
  char detectedLat[16] = "";
  char detectedLon[16] = "";
  bool wifiConnectedOnEnter = false;    // WiFi was connected before this activity started
  bool wifiEnabledForActivity = false;  // true when this activity brought WiFi up itself
  bool orientationApplied = false;      // true when the weather orientation was applied
  GfxRenderer::Orientation originalOrientation = GfxRenderer::Portrait;

  int cityCursor = 0;      // Cursor in city list (0=Auto, 1..CITY_COUNT=cities)
  int cityScrollTop = 0;   // First visible item in city list

  void onWifiConnected();
  bool beginSilentWifiConnect();  // non-blocking; polled from loop() while state == WIFI_CONNECTING
  void goToWifiSelection();
  void fetchWeather();
  bool parseWeather(const std::string& json);
  bool fetchAirQuality(const char* lat, const char* lon);
  bool detectLocation();
  const char* getCurrentCityName() const;
  void renderCityList();
};