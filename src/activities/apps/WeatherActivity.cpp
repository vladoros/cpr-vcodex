#include "WeatherActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/icons/weather.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/RefreshInterval.h"
#include "util/TimeUtils.h"

namespace {
void applyWeatherOrientation(GfxRenderer& renderer) {
  switch (SETTINGS.weatherOrientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}
}  // namespace

// Scroll indicator helpers (defined below; forward-declared for renderCityList).
static void drawTriangleUp(GfxRenderer& r, const int cx, const int y);
static void drawTriangleDown(GfxRenderer& r, const int cx, const int y);

// Cities mirror TimeZoneRegistry::TIME_ZONE_PRESETS (excluding UTC), in the same order.
// Names use the short city form shown in parentheses in the preset labels.
const CityCoord WeatherActivity::CITIES[CITY_COUNT] = {
    {"Madrid", "40.4168", "-3.7038"},
    {"London", "51.5074", "-0.1278"},
    {"Paris", "48.8566", "2.3522"},
    {"Berlin", "52.5200", "13.4050"},
    {"Rome", "41.9028", "12.4964"},
    {"Athens", "37.9838", "23.7275"},
    {"Helsinki", "60.1699", "24.9384"},
    {"Moscow", "55.7558", "37.6173"},
    {"New York", "40.7128", "-74.0060"},
    {"Chicago", "41.8781", "-87.6298"},
    {"Denver", "39.7392", "-104.9903"},
    {"Los Angeles", "34.0522", "-118.2437"},
    {"Mexico City", "19.4326", "-99.1332"},
    {"Sao Paulo", "-23.5505", "-46.6333"},
    {"Buenos Aires", "-34.6037", "-58.3816"},
    {"Johannesburg", "-26.2041", "28.0473"},
    {"Nairobi", "-1.2921", "36.8219"},
    {"Dubai", "25.2048", "55.2708"},
    {"Karachi", "24.8607", "67.0011"},
    {"New Delhi", "28.6139", "77.2090"},
    {"Dhaka", "23.8103", "90.4125"},
    {"Bangkok", "13.7563", "100.5018"},
    {"Singapore", "1.3521", "103.8198"},
    {"Beijing", "39.9042", "116.4074"},
    {"Tokyo", "35.6762", "139.6503"},
    {"Seoul", "37.5665", "126.9780"},
    {"Sydney", "-33.8688", "151.2093"},
    {"Auckland", "-36.8485", "174.7633"},
};

// Silent WiFi connect timeout, polled from loop() rather than blocked on.
constexpr unsigned long WIFI_SILENT_CONNECT_TIMEOUT_MS = 10000;

void WeatherActivity::onEnter() {
  Activity::onEnter();
  originalOrientation = renderer.getOrientation();
  applyWeatherOrientation(renderer);
  orientationApplied = true;
  renderer.requestNextFullRefresh();
  selectedCity = SETTINGS.weatherCity;
  if (selectedCity > CITY_COUNT) selectedCity = 0;  // 0=Auto, 1..CITY_COUNT=manual
  lastUpdateTime[0] = '\0';
  detectedCityName[0] = '\0';
  detectedLat[0] = '\0';
  detectedLon[0] = '\0';
  wifiConnectedOnEnter = WiFi.status() == WL_CONNECTED;
  wifiEnabledForActivity = false;

  state = WIFI_CONNECTING;
  statusMessage = tr(STR_FETCHING_WEATHER);
  requestUpdate(true);

  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected();
  } else {
    wifiEnabledForActivity = true;
    if (!beginSilentWifiConnect()) {
      // No saved credentials to try silently; go straight to the picker.
      goToWifiSelection();
    }
    // Otherwise stay in WIFI_CONNECTING; loop() polls the connection result.
  }
}

// Starts a saved-credential WiFi connect without blocking. Returns false
// (no side effects) when there's nothing to try, so the caller can fall
// back to the interactive picker immediately.
bool WeatherActivity::beginSilentWifiConnect() {
  const auto& ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) return false;

  const std::optional<WifiCredential> cred = WIFI_STORE.findCredential(ssid);
  if (!cred) return false;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  wifiConnectStartMs = millis();
  return true;
}

void WeatherActivity::goToWifiSelection() {
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
      [this](const ActivityResult& r) {
        if (r.isCancelled) {
          finish();
          return;
        }
        onWifiConnected();
      });
}

void WeatherActivity::onWifiConnected() {
  // Auto-detect location by IP if selectedCity == 0
  if (selectedCity == 0 && !detectLocation()) {
    // Fallback to first manual city if geolocation fails
    LOG_ERR("WEATHER", "IP geolocation failed, falling back to manual city");
    selectedCity = 1;
  }

  state = FETCHING;
  statusMessage = tr(STR_FETCHING_WEATHER);
  requestUpdate(true);
  fetchWeather();
}

bool WeatherActivity::detectLocation() {
  std::string response;
  if (!HttpDownloader::fetchUrl("http://ip-api.com/json/?fields=lat,lon,city", response)) {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;

  float lat = doc["lat"] | 0.0f;
  float lon = doc["lon"] | 0.0f;
  const char* city = doc["city"] | "";

  if (lat == 0.0f && lon == 0.0f) return false;

  snprintf(detectedLat, sizeof(detectedLat), "%.4f", lat);
  snprintf(detectedLon, sizeof(detectedLon), "%.4f", lon);
  strncpy(detectedCityName, city, sizeof(detectedCityName) - 1);
  detectedCityName[sizeof(detectedCityName) - 1] = '\0';

  LOG_DBG("WEATHER", "IP geolocation: %s (%.4f, %.4f)", detectedCityName, lat, lon);
  return true;
}

const char* WeatherActivity::getCurrentCityName() const {
  if (selectedCity == 0) {
    return detectedCityName[0] ? detectedCityName : "Auto";
  }
  return CITIES[selectedCity - 1].name;
}

void WeatherActivity::fetchWeather() {
  lastFetchMs = millis();  // stamp at start so failed/ongoing fetches throttle the next attempt

  const char* lat;
  const char* lon;

  if (selectedCity == 0 && detectedLat[0]) {
    lat = detectedLat;
    lon = detectedLon;
  } else {
    int idx = (selectedCity > 0) ? selectedCity - 1 : 0;
    lat = CITIES[idx].lat;
    lon = CITIES[idx].lon;
  }

  char url[384];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?"
           "latitude=%s&longitude=%s"
           "&current=temperature_2m,relative_humidity_2m,"
           "apparent_temperature,weather_code,wind_speed_10m,"
           "uv_index,surface_pressure,dew_point_2m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "sunrise,sunset"
           "&forecast_days=6&timezone=auto",
           lat, lon);

  std::string response;
  if (!HttpDownloader::fetchUrl(std::string(url), response)) {
    state = FETCH_ERROR;
    statusMessage = tr(STR_WEATHER_ERROR);
    requestUpdate(true);
    return;
  }

  if (!parseWeather(response)) {
    state = FETCH_ERROR;
    statusMessage = tr(STR_WEATHER_ERROR);
    requestUpdate(true);
    return;
  }

  // Best-effort air quality; leave card blank if it fails rather than showing a stale reading
  weather.airQuality = -1;
  if (!fetchAirQuality(lat, lon)) {
    LOG_DBG("WEATHER", "Air quality fetch failed or unavailable");
  }

  // Cache last-known temperature for the device top bar. Clamp away from
  // WEATHER_TEMP_UNAVAILABLE so a real (if extreme) reading is never
  // mistaken for "no data".
  int8_t cachedTempC = static_cast<int8_t>(lroundf(weather.temperature));
  if (cachedTempC == CrossPointSettings::WEATHER_TEMP_UNAVAILABLE) cachedTempC++;
  SETTINGS.weatherLastTempC = cachedTempC;

  // Show the update time in device-local time. There is no RTC on the X4, so
  // use the live system clock via the same timezone path as the header; the
  // label must always appear after a successful fetch.
  TimeUtils::configureTimezone();
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);
  snprintf(lastUpdateTime, sizeof(lastUpdateTime), "%02d:%02d", ti.tm_hour, ti.tm_min);

  state = DISPLAYING;
  requestUpdate(true);
}

bool WeatherActivity::parseWeather(const std::string& json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WEATHER", "JSON parse error: %s", error.c_str());
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull()) return false;

  weather.temperature = current["temperature_2m"] | 0.0f;
  weather.feelsLike = current["apparent_temperature"] | 0.0f;
  weather.humidity = current["relative_humidity_2m"] | 0;
  weather.weatherCode = current["weather_code"] | 0;
  weather.windSpeed = current["wind_speed_10m"] | 0.0f;
  weather.dewPoint = current["dew_point_2m"] | 0.0f;
  weather.pressure = current["surface_pressure"] | 0.0f;
  weather.uvIndex = current["uv_index"] | 0.0f;

  // Parse daily forecast (skip day 0 = today, show next 5 days)
  forecastCount = 0;
  JsonObject daily = doc["daily"];
  if (!daily.isNull()) {
    static const char* DAY_NAMES[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    JsonArray codes = daily["weather_code"];
    JsonArray maxTemps = daily["temperature_2m_max"];
    JsonArray minTemps = daily["temperature_2m_min"];
    JsonArray dates = daily["time"];
    JsonArray sunrises = daily["sunrise"];
    JsonArray sunsets = daily["sunset"];

    // Sunrise/sunset (today, ISO "YYYY-MM-DDTHH:MM" in city local time)
    if (!sunrises.isNull() && !sunsets.isNull()) {
      const char* sunrise = sunrises[0].as<const char*>();
      const char* sunset = sunsets[0].as<const char*>();
      if (sunrise && strlen(sunrise) >= 16) {
        memcpy(weather.sunrise, sunrise + 11, 5);
        weather.sunrise[5] = '\0';
      }
      if (sunset && strlen(sunset) >= 16) {
        memcpy(weather.sunset, sunset + 11, 5);
        weather.sunset[5] = '\0';
      }
    }

    for (int i = 1; i < (int)codes.size() && forecastCount < FORECAST_DAYS; i++) {
      auto& f = forecast[forecastCount];
      f.weatherCode = codes[i] | 0;
      f.tempMax = maxTemps[i] | 0.0f;
      f.tempMin = minTemps[i] | 0.0f;
      // Parse day of week from date string "YYYY-MM-DD"
      const char* dateStr = dates[i] | "";
      struct tm tm = {};
      if (sscanf(dateStr, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        mktime(&tm);
        strncpy(f.dayLabel, DAY_NAMES[tm.tm_wday], sizeof(f.dayLabel) - 1);
      }
      forecastCount++;
    }
  }
  return true;
}

// Best-effort US AQI (air-quality-api.open-meteo.com). Keeps weather.airQuality = -1 when unavailable.
bool WeatherActivity::fetchAirQuality(const char* lat, const char* lon) {
  char url[192];
  snprintf(url, sizeof(url),
           "http://air-quality-api.open-meteo.com/v1/air-quality?"
           "latitude=%s&longitude=%s&current=us_aqi",
           lat, lon);

  std::string response;
  if (!HttpDownloader::fetchUrl(std::string(url), response)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;
  JsonObject current = doc["current"];
  if (current.isNull() || !current["us_aqi"].is<int>()) return false;

  const int aqi = current["us_aqi"].as<int>();
  if (aqi < 0) return false;
  weather.airQuality = aqi;
  return true;
}

const char* WeatherActivity::weatherCodeToString(int code) {
  if (code == 0) return tr(STR_WEATHER_CLEAR);
  if (code <= 3) return tr(STR_WEATHER_PARTLY_CLOUDY);
  if (code <= 48) return tr(STR_WEATHER_FOG);
  if (code <= 57) return tr(STR_WEATHER_DRIZZLE);
  if (code <= 67) return tr(STR_WEATHER_RAIN);
  if (code <= 77) return tr(STR_WEATHER_SNOW);
  if (code <= 82) return tr(STR_WEATHER_SHOWERS);
  return tr(STR_WEATHER_THUNDERSTORM);
}

void WeatherActivity::onExit() {
  SETTINGS.weatherCity = selectedCity;
  SETTINGS.saveToFile();
  if (orientationApplied) {
    renderer.setOrientation(originalOrientation);
    renderer.requestNextFullRefresh();
    orientationApplied = false;
  }
  if (!wifiConnectedOnEnter && wifiEnabledForActivity) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }
  Activity::onExit();
}

void WeatherActivity::loop() {
  auto back = mappedInput.wasReleased(MappedInputManager::Button::Back);
  auto confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  auto next = mappedInput.wasReleased(MappedInputManager::Button::Right);
  auto prev = mappedInput.wasReleased(MappedInputManager::Button::Left);
  auto up = mappedInput.wasReleased(MappedInputManager::Button::Up);
  auto down = mappedInput.wasReleased(MappedInputManager::Button::Down);

  if (state == SELECTING_CITY) {
    int total = CITY_COUNT + 1;  // 0=Auto + CITY_COUNT cities
    if (back) {
      state = DISPLAYING;
      requestUpdate();
      return;
    }
    if (down || next) {
      cityCursor = (cityCursor + 1) % total;
      // Keep cursor visible in scroll window
      if (cityCursor < cityScrollTop) cityScrollTop = cityCursor;
      if (cityCursor >= cityScrollTop + 10) cityScrollTop = cityCursor - 9;
      requestUpdate();
    }
    if (up || prev) {
      cityCursor = (cityCursor + total - 1) % total;
      if (cityCursor < cityScrollTop) cityScrollTop = cityCursor;
      if (cityCursor >= cityScrollTop + 10) cityScrollTop = cityCursor - 9;
      requestUpdate();
    }
    if (confirm) {
      selectedCity = cityCursor;
      if (selectedCity == 0 && detectedLat[0] == '\0' && !detectLocation()) {
        // Fallback to first manual city if geolocation fails, so the
        // displayed city name matches the data actually shown.
        LOG_ERR("WEATHER", "IP geolocation failed, falling back to manual city");
        selectedCity = 1;
      }
      state = FETCHING;
      statusMessage = tr(STR_FETCHING_WEATHER);
      requestUpdate(true);
      fetchWeather();
    }
    return;
  }

  if (back) {
    finish();
    return;
  }

  if (state == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      onWifiConnected();
      return;
    }
    if (millis() - wifiConnectStartMs >= WIFI_SILENT_CONNECT_TIMEOUT_MS) {
      // Silent reconnect using saved credentials timed out; fall back to
      // the interactive picker instead of blocking the UI further.
      WiFi.disconnect(false);
      goToWifiSelection();
    }
    return;
  }

  if (state == DISPLAYING || state == FETCH_ERROR) {
    auto startRefresh = [this]() {
      state = FETCHING;
      statusMessage = tr(STR_FETCHING_WEATHER);
      requestUpdate(true);
      fetchWeather();
    };

    if (confirm) {
      startRefresh();
      return;
    }
    if (next || prev) {
      // Open city selection list
      cityCursor = selectedCity;
      cityScrollTop = (cityCursor > 5) ? cityCursor - 5 : 0;
      state = SELECTING_CITY;
      requestUpdate();
      return;
    }

    // Auto-refresh while open at the configured interval (mirrors WebDash viewer)
    if (lastFetchMs != 0 && millis() - lastFetchMs >= refreshIntervalMs(SETTINGS.weatherRefreshInterval)) {
      startRefresh();
    }
  }
}

void WeatherActivity::renderCityList() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CITY));

  int y = metrics.topPadding + metrics.headerHeight + 10;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 6;
  const int total = CITY_COUNT + 1;
  const int visibleCount = 10;

  for (int i = 0; i < visibleCount && (cityScrollTop + i) < total; i++) {
    int idx = cityScrollTop + i;
    const char* name = (idx == 0) ? "Auto" : CITIES[idx - 1].name;

    if (idx == cityCursor) {
      // Highlight selected row
      renderer.fillRect(5, y - 2, pageWidth - 10, lineH, true);
      renderer.drawText(UI_10_FONT_ID, 15, y, name, false);  // white text on black
      if (idx == selectedCity) {
        renderer.drawText(UI_10_FONT_ID, pageWidth - 40, y, "*", false);
      }
    } else {
      renderer.drawText(UI_10_FONT_ID, 15, y, name, true);
      if (idx == selectedCity) {
        renderer.drawText(UI_10_FONT_ID, pageWidth - 40, y, "*", true);
      }
    }
    y += lineH;
  }

  // Scroll indicators
  if (cityScrollTop > 0) {
    drawTriangleUp(renderer, pageWidth / 2, metrics.topPadding + metrics.headerHeight);
  }
  if (cityScrollTop + visibleCount < total) {
    drawTriangleDown(renderer, pageWidth / 2, y + 2);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// Draw a 32x32 icon at 1x using drawPixel (orientation-aware, unlike drawIcon)
static void drawIcon1x(GfxRenderer& r, const uint8_t* icon, int x, int y) {
  for (int row = 0; row < 32; row++) {
    for (int col = 0; col < 32; col++) {
      const int byteIdx = row * 4 + col / 8;
      const int bitIdx = 7 - (col % 8);
      if (!((icon[byteIdx] >> bitIdx) & 1)) {
        r.drawPixel(x + col, y + row);
      }
    }
  }
}

// Draw a 32x32 icon scaled 2x (64x64) using pixel doubling
static void drawIconScaled2x(GfxRenderer& r, const uint8_t* icon, int x, int y) {
  for (int row = 0; row < 32; row++) {
    for (int col = 0; col < 32; col++) {
      int byteIdx = row * 4 + col / 8;
      int bitIdx = 7 - (col % 8);
      bool isBlack = !((icon[byteIdx] >> bitIdx) & 1);
      if (isBlack) {
        int dx = x + col * 2;
        int dy = y + row * 2;
        r.drawPixel(dx, dy);
        r.drawPixel(dx + 1, dy);
        r.drawPixel(dx, dy + 1);
        r.drawPixel(dx + 1, dy + 1);
      }
    }
  }
}

// Small scroll indicator triangles (the font subset has no ▲/▼ glyphs).
// cx is the horizontal center, y the triangle tip (up) / top baseline (down).
static void drawTriangleUp(GfxRenderer& r, const int cx, const int y) {
  r.drawLine(cx - 4, y + 6, cx, y, true);
  r.drawLine(cx, y, cx + 4, y + 6, true);
  r.drawLine(cx - 4, y + 6, cx + 4, y + 6, true);
}

static void drawTriangleDown(GfxRenderer& r, const int cx, const int y) {
  r.drawLine(cx - 4, y, cx, y + 6, true);
  r.drawLine(cx, y + 6, cx + 4, y, true);
  r.drawLine(cx - 4, y, cx + 4, y, true);
}

void WeatherActivity::render(RenderLock&&) {
  if (state == SELECTING_CITY) {
    renderCityList();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  // Landscape viewport is 480 tall; use a dedicated wide layout instead of the portrait stack.
  const bool compact = pageHeight < 550;

  renderer.clearScreen();

  if (state == FETCHING || state == WIFI_CONNECTING || state == FETCH_ERROR) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEATHER));
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, statusMessage.c_str());
  } else if (compact) {
    // Landscape: fills the wide screen; grid replaces the tall card.
    char buf[64];
    int tw = 0;
    int y = metrics.topPadding + 8;

    // City name — centered bold
    renderer.drawCenteredText(UI_12_FONT_ID, y, getCurrentCityName(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 4;

    // Weather description
    renderer.drawCenteredText(UI_10_FONT_ID, y, weatherCodeToString(weather.weatherCode));
    y += renderer.getLineHeight(UI_10_FONT_ID) + 8;

    // Large icon + temperature side by side, centered as a cluster
    snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(weather.temperature), tempUnitSuffix());
    const int tempW = renderer.getTextWidth(NOTOSANS_18_FONT_ID, buf);
    const int tempH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
    const int iconSize = WEATHER_ICON_SIZE * 2;  // 64x64 via 2x scaling
    const int clusterW = iconSize + 16 + tempW;
    const int clusterX = (pageWidth - clusterW) / 2;
    drawIconScaled2x(renderer, getWeatherIcon(weather.weatherCode), clusterX, y);
    renderer.drawText(NOTOSANS_18_FONT_ID, clusterX + iconSize + 16, y + (iconSize - tempH) / 2, buf, true,
                      EpdFontFamily::BOLD);
    y += iconSize + 12;

    // Details: 4 columns x 2 rows — compact and unboxed, no card border
    const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
    const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
    const int statX = 16;
    const int statW = pageWidth - statX * 2;
    const int colW = statW / 4;
    const int rowH = labelH + valueH + 6;
    const int statTop = y;
    auto drawStat = [&](int col, int row, const char* label, const char* value) {
      const int cx = statX + col * colW;
      const int cy = statTop + row * rowH;
      tw = renderer.getTextWidth(SMALL_FONT_ID, label);
      renderer.drawText(SMALL_FONT_ID, cx + (colW - tw) / 2, cy, label);
      if (value[0] != '\0') {
        tw = renderer.getTextWidth(UI_12_FONT_ID, value);
        renderer.drawText(UI_12_FONT_ID, cx + (colW - tw) / 2, cy + labelH + 3, value, true, EpdFontFamily::BOLD);
      }
    };

    snprintf(buf, sizeof(buf), "%d%%", weather.humidity);
    drawStat(0, 0, tr(STR_HUMIDITY), buf);
    snprintf(buf, sizeof(buf), "%.1f km/h", weather.windSpeed);
    drawStat(1, 0, tr(STR_WIND), buf);
    snprintf(buf, sizeof(buf), "%.0f", weather.uvIndex);
    drawStat(2, 0, tr(STR_UV_INDEX), buf);
    if (weather.airQuality >= 0) {
      snprintf(buf, sizeof(buf), "%d", weather.airQuality);
    } else {
      buf[0] = '\0';
    }
    drawStat(3, 0, tr(STR_AIR_QUALITY), buf);
    snprintf(buf, sizeof(buf), "%.0f hPa", weather.pressure);
    drawStat(0, 1, tr(STR_PRESSURE), buf);
    if (weather.sunrise[0] && weather.sunset[0]) {
      snprintf(buf, sizeof(buf), "%s / %s", weather.sunrise, weather.sunset);
    } else {
      buf[0] = '\0';
    }
    drawStat(1, 1, tr(STR_SUNRISE_SUNSET), buf);
    snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(weather.dewPoint), tempUnitSuffix());
    drawStat(2, 1, tr(STR_DEW_POINT), buf);
    snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(weather.feelsLike), tempUnitSuffix());
    drawStat(3, 1, tr(STR_FEELS_LIKE), buf);
    y = statTop + rowH * 2 + 12;

    // 5-day forecast row
    if (forecastCount > 0) {
      renderer.drawLine(statX, y, statX + statW, y, true);
      y += 8;
      const int cols = forecastCount;
      const int cellW = statW / cols;
      for (int i = 0; i < cols; i++) {
        const auto& f = forecast[i];
        const int cx = statX + i * cellW + cellW / 2;
        tw = renderer.getTextWidth(SMALL_FONT_ID, f.dayLabel);
        renderer.drawText(SMALL_FONT_ID, cx - tw / 2, y, f.dayLabel);
        drawIcon1x(renderer, getWeatherIcon(f.weatherCode), cx - WEATHER_ICON_SIZE / 2, y + 16);
        snprintf(buf, sizeof(buf), "%.0f%s / %.0f%s", convertTemp(f.tempMax), tempUnitSuffix(), convertTemp(f.tempMin),
                 tempUnitSuffix());
        tw = renderer.getTextWidth(SMALL_FONT_ID, buf);
        renderer.drawText(SMALL_FONT_ID, cx - tw / 2, y + 52, buf, true, EpdFontFamily::BOLD);
      }
      y += 76;
    }

    // Spacing between a label and the next element, matching the gap used after "Feels like".
    y += renderer.getLineHeight(UI_10_FONT_ID) + 16;

    // Last updated
    if (lastUpdateTime[0]) {
      snprintf(buf, sizeof(buf), "%s: %s", tr(STR_LAST_UPDATED), lastUpdateTime);
      renderer.drawCenteredText(SMALL_FONT_ID, y, buf);
    }
  } else {
    // Portrait: vertical stack with the details card.
    char buf[64];
    int tw = 0;
    int y = metrics.topPadding + 16;

    // City name — centered bold
    renderer.drawCenteredText(UI_12_FONT_ID, y, getCurrentCityName(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 2;

    // Weather description
    renderer.drawCenteredText(UI_10_FONT_ID, y, weatherCodeToString(weather.weatherCode));
    y += renderer.getLineHeight(UI_10_FONT_ID) + 12;

    // Large weather icon (64x64 via 2x scaling) — centered
    const int iconSize = WEATHER_ICON_SIZE * 2;
    drawIconScaled2x(renderer, getWeatherIcon(weather.weatherCode), (pageWidth - iconSize) / 2, y);
    y += iconSize + 8;

    // Large temperature
    snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(weather.temperature), tempUnitSuffix());
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, y, buf, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 4;

    // Feels like
    snprintf(buf, sizeof(buf), "%s %.0f%s", tr(STR_FEELS_LIKE), convertTemp(weather.feelsLike), tempUnitSuffix());
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 16;

    // Details card — 2 columns x 4 rows of details
    const int cardX = 20;
    const int cardW = pageWidth - 40;
    const int colW = cardW / 2;

    // Lay rows out from the actual font metrics so label/value never cross the dividers.
    const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
    const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
    const int labelValueGap = 4;
    const int rowH = labelH + valueH + labelValueGap + 6;  // +4px clearance under value + over next label
    const int outerPad = 6;
    const int cardH = outerPad * 2 + rowH * 4;
    const int contentTop = y + outerPad;
    renderer.drawRoundedRect(cardX, y, cardW, cardH, 1, 8, true);

    // Vertical divider between columns
    renderer.drawLine(cardX + colW, contentTop + 2, cardX + colW, contentTop + rowH * 4 - 2, true);
    // Horizontal dividers between rows
    for (int r = 1; r < 4; r++) {
      renderer.drawLine(cardX + 8, contentTop + r * rowH, cardX + cardW - 8, contentTop + r * rowH, true);
    }

    auto drawDetailCell = [&](int col, int row, const char* label, const char* value) {
      const int cx = cardX + col * colW;
      const int cy = contentTop + row * rowH + 2;
      int tw = renderer.getTextWidth(SMALL_FONT_ID, label);
      renderer.drawText(SMALL_FONT_ID, cx + (colW - tw) / 2, cy, label);
      if (value[0] != '\0') {
        tw = renderer.getTextWidth(UI_12_FONT_ID, value);
        renderer.drawText(UI_12_FONT_ID, cx + (colW - tw) / 2, cy + labelH + labelValueGap, value, true,
                          EpdFontFamily::BOLD);
      }
    };

    // Humidity
    snprintf(buf, sizeof(buf), "%d%%", weather.humidity);
    drawDetailCell(1, 0, tr(STR_HUMIDITY), buf);
    // Feels like
    snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(weather.feelsLike), tempUnitSuffix());
    drawDetailCell(0, 0, tr(STR_FEELS_LIKE), buf);
    // Wind
    snprintf(buf, sizeof(buf), "%.1f km/h", weather.windSpeed);
    drawDetailCell(0, 1, tr(STR_WIND), buf);
    // UV index
    snprintf(buf, sizeof(buf), "%.0f", weather.uvIndex);
    drawDetailCell(1, 1, tr(STR_UV_INDEX), buf);
    // Air quality (blank when unavailable)
    if (weather.airQuality >= 0) {
      snprintf(buf, sizeof(buf), "%d", weather.airQuality);
    } else {
      buf[0] = '\0';
    }
    drawDetailCell(0, 2, tr(STR_AIR_QUALITY), buf);
    // Pressure
    snprintf(buf, sizeof(buf), "%.0f hPa", weather.pressure);
    drawDetailCell(1, 2, tr(STR_PRESSURE), buf);
    // Sunrise/Set (city local time, 24h)
    if (weather.sunrise[0] && weather.sunset[0]) {
      snprintf(buf, sizeof(buf), "%s / %s", weather.sunrise, weather.sunset);
    } else {
      buf[0] = '\0';
    }
    drawDetailCell(0, 3, tr(STR_SUNRISE_SUNSET), buf);
    // Dew point
    snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(weather.dewPoint), tempUnitSuffix());
    drawDetailCell(1, 3, tr(STR_DEW_POINT), buf);

    y += cardH + 12;

    // 5-day forecast row
    if (forecastCount > 0) {
      renderer.drawLine(cardX, y, cardX + cardW, y, true);
      y += 8;
      const int cols = forecastCount;
      const int cellW = cardW / cols;
      for (int i = 0; i < cols; i++) {
        const auto& f = forecast[i];
        int cx = cardX + i * cellW + cellW / 2;

        // Day label centered
        tw = renderer.getTextWidth(SMALL_FONT_ID, f.dayLabel);
        renderer.drawText(SMALL_FONT_ID, cx - tw / 2, y, f.dayLabel);

        // Small icon centered (32x32)
        const uint8_t* fIcon = getWeatherIcon(f.weatherCode);
        drawIcon1x(renderer, fIcon, cx - WEATHER_ICON_SIZE / 2, y + 20);

        // High/Low temp
        snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(f.tempMax), tempUnitSuffix());
        tw = renderer.getTextWidth(SMALL_FONT_ID, buf);
        renderer.drawText(SMALL_FONT_ID, cx - tw / 2, y + 56, buf, true, EpdFontFamily::BOLD);

        snprintf(buf, sizeof(buf), "%.0f%s", convertTemp(f.tempMin), tempUnitSuffix());
        tw = renderer.getTextWidth(SMALL_FONT_ID, buf);
        renderer.drawText(SMALL_FONT_ID, cx - tw / 2, y + 74, buf);
      }
      y += 96;
    }

    // Spacing between a label and the next element, matching the gap used after "Feels like".
    y += renderer.getLineHeight(UI_10_FONT_ID) + 16;

    // Last updated
    if (lastUpdateTime[0]) {
      snprintf(buf, sizeof(buf), "%s: %s", tr(STR_LAST_UPDATED), lastUpdateTime);
      renderer.drawCenteredText(SMALL_FONT_ID, y, buf);
    }
  }

  // Button hints: Back | Refresh | Location (Left+Right both open city picker)
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_REFRESH), tr(STR_LOCATION), tr(STR_LOCATION));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}