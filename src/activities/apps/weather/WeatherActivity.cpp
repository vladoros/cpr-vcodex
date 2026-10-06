#include "WeatherActivity.h"

#if CROSSINK_APP_CAP_WEATHER

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WeatherParser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "WeatherCacheStore.h"
#include "activities/RenderLock.h"
#include "activities/apps/AppChrome.h"
#include "activities/apps/AppFonts.h"
#include "activities/apps/AppNetwork.h"
#include "activities/apps/AppOptionsActivity.h"
#include "activities/apps/AppRefreshInterval.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/weatherIcons.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr uint8_t kFullRefreshEvery = 12;
constexpr size_t kResponseReserve = 2048;
constexpr unsigned kTemperaturePoints = 28;
// A landscape viewport is 480 tall; below this the wide grid layout is used.
constexpr int kCompactLayoutMaxHeight = 550;

constexpr StrId kWeekdayIds[] = {StrId::STR_WEEKDAY_SUN_SHORT, StrId::STR_WEEKDAY_MON_SHORT,
                                 StrId::STR_WEEKDAY_TUE_SHORT, StrId::STR_WEEKDAY_WED_SHORT,
                                 StrId::STR_WEEKDAY_THU_SHORT, StrId::STR_WEEKDAY_FRI_SHORT,
                                 StrId::STR_WEEKDAY_SAT_SHORT};

const char* conditionLabel(const int code) {
  switch (WeatherParser::conditionForCode(code)) {
    case WeatherCondition::Clear:
      return tr(STR_WEATHER_CLEAR);
    case WeatherCondition::PartlyCloudy:
      return tr(STR_WEATHER_PARTLY_CLOUDY);
    case WeatherCondition::Fog:
      return tr(STR_WEATHER_FOG);
    case WeatherCondition::Drizzle:
      return tr(STR_WEATHER_DRIZZLE);
    case WeatherCondition::Rain:
      return tr(STR_WEATHER_RAIN);
    case WeatherCondition::Snow:
      return tr(STR_WEATHER_SNOW);
    case WeatherCondition::Showers:
      return tr(STR_WEATHER_SHOWERS);
    case WeatherCondition::Thunderstorm:
      return tr(STR_WEATHER_THUNDERSTORM);
  }
  return "";
}

const freeink::Icon& conditionIcon(const int code, const bool large) {
  switch (WeatherParser::conditionForCode(code)) {
    case WeatherCondition::Clear:
      return large ? icon_wx_clear_64 : icon_wx_clear_32;
    case WeatherCondition::PartlyCloudy:
      // WMO 3 is overcast; 1-2 are mostly clear / partly cloudy.
      if (code >= 3) return large ? icon_wx_cloud_64 : icon_wx_cloud_32;
      return large ? icon_wx_partly_cloudy_64 : icon_wx_partly_cloudy_32;
    case WeatherCondition::Fog:
      return large ? icon_wx_fog_64 : icon_wx_fog_32;
    case WeatherCondition::Drizzle:
    case WeatherCondition::Rain:
    case WeatherCondition::Showers:
      return large ? icon_wx_rain_64 : icon_wx_rain_32;
    case WeatherCondition::Snow:
      return large ? icon_wx_snow_64 : icon_wx_snow_32;
    case WeatherCondition::Thunderstorm:
      return large ? icon_wx_storm_64 : icon_wx_storm_32;
  }
  return large ? icon_wx_cloud_64 : icon_wx_cloud_32;
}

// Rounds through an integer so -0.4 shows as "0°", not "-0°".
void formatTemp(const float celsius, char* out, const size_t outSize) {
  snprintf(out, outSize, "%ld\xC2\xB0", lroundf(celsius));
}

void formatCityChoice(const uint8_t index, char* out, const size_t outSize) {
  if (index == 0 || index > WeatherParser::kCityCount) {
    snprintf(out, outSize, "%s", tr(STR_WEATHER_CITY_AUTO));
  } else {
    snprintf(out, outSize, "%s", WeatherParser::kCities[index - 1].name);
  }
}

void formatIntervalChoice(const uint8_t index, char* out, const size_t outSize) {
  AppOptionFormat::minutes(AppRefreshInterval::minutesAt(AppRefreshInterval::kWeatherMinutes, index), out, outSize);
}

constexpr AppOptionRow kOptionRows[] = {
    {AppOptionRow::Kind::Choice, StrId::STR_WEATHER_CITY, &CrossPointSettings::weatherCity,
     CrossPointSettings::APP_WEATHER_CITY_CHOICES, &formatCityChoice},
    {AppOptionRow::Kind::Choice, StrId::STR_REFRESH_INTERVAL, &CrossPointSettings::weatherRefreshInterval,
     CrossPointSettings::APP_WEATHER_INTERVAL_CHOICES, &formatIntervalChoice},
    {AppOptionRow::Kind::Choice, StrId::STR_ORIENTATION, &CrossPointSettings::weatherOrientation,
     CrossPointSettings::ORIENTATION_COUNT, &AppOptionFormat::orientation},
    {AppOptionRow::Kind::Toggle, StrId::STR_WEATHER_TOPBAR, &CrossPointSettings::weatherTopbarEnabled},
};
constexpr int kOptionRowCount = static_cast<int>(sizeof(kOptionRows) / sizeof(kOptionRows[0]));

template <size_t N>
void copyStr(char (&dst)[N], const char* src) {
  snprintf(dst, N, "%s", src ? src : "");
}
}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();
  // TLS is not used here, but the Wi-Fi stack itself needs the room on a C3.
  sdFontSystem.releaseForNetwork(renderer);
  originalOrientation_ = renderer.getOrientation();
  applyOrientation();
  displayFontId_ = appDisplayFontId(renderer, kTemperaturePoints);
  if (SETTINGS.weatherCity >= CrossPointSettings::APP_WEATHER_CITY_CHOICES) SETTINGS.weatherCity = 0;

  response_.reserve(kResponseReserve);
  detectedLat_[0] = detectedLon_[0] = detectedCity_[0] = '\0';
  lastFetchMs_ = 0;
  wifiOwned_ = false;
  topbarDirty_ = false;
  hasData_ = false;
  showingCache_ = false;

  WEATHER_CACHE.ensureLoaded();
  loadCache();
  startRefresh(/*userInitiated=*/true);
}

void WeatherActivity::onExit() {
  if (wifiOwned_) AppNetwork::tearDown();
  if (topbarDirty_) SETTINGS.saveToFile();
  // ActivityManager holds the render lock around onExit.
  renderer.setOrientation(originalOrientation_);
  std::string().swap(response_);
  Activity::onExit();
}

void WeatherActivity::applyOrientation() {
  RenderLock lock(*this);
  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(
      SETTINGS.weatherOrientation < CrossPointSettings::ORIENTATION_COUNT ? SETTINGS.weatherOrientation : 0));
  forceHalfRefresh_ = true;
}

void WeatherActivity::exitApp() { finishAfterBackPress(); }

void WeatherActivity::loadCache() {
  if (!WEATHER_CACHE.hasCache()) return;
  weather_ = WEATHER_CACHE.getWeather();
  forecastCount_ = std::min(WEATHER_CACHE.getForecastCount(), WEATHER_FORECAST_DAYS);
  for (int i = 0; i < forecastCount_; i++) forecast_[i] = WEATHER_CACHE.getForecast()[i];
  copyStr(cityName_, WEATHER_CACHE.getCityName());
  copyStr(updatedAt_, WEATHER_CACHE.getUpdatedAt());
  hasData_ = true;
  showingCache_ = true;
}

void WeatherActivity::showCacheOrError() {
  RenderLock lock(*this);
  // A failed refresh keeps the last good reading on screen, labelled as cached.
  if (hasData_) {
    showingCache_ = true;
  } else {
    loadCache();
  }
  state_ = hasData_ ? State::Displaying : State::Error;
  requestUpdate(true);
}

void WeatherActivity::startRefresh(const bool userInitiated) {
  userRequestedFetch_ = userInitiated;
  if (AppNetwork::isConnected()) {
    state_ = State::Fetching;
    requestUpdate(true);
    return;
  }
  if (AppNetwork::beginSilentConnect()) {
    wifiOwned_ = true;
    connectStartMs_ = millis();
    state_ = State::Connecting;
    requestUpdate(true);
    return;
  }
  onConnectFailed(userInitiated);
}

void WeatherActivity::onConnectFailed(const bool userInitiated) {
  // A timer-driven refresh never pops the Wi-Fi picker; neither does a cached
  // screen on entry, which is more useful than a picker the user did not ask for.
  if (!userInitiated || (hasData_ && lastFetchMs_ == 0)) {
    lastFetchMs_ = millis();
    showCacheOrError();
    return;
  }
  openWifiSelection();
}

void WeatherActivity::openWifiSelection() {
  wifiOwned_ = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || !AppNetwork::isConnected()) {
                             lastFetchMs_ = millis();
                             if (!hasData_ && !WEATHER_CACHE.hasCache()) {
                               finish();
                               return;
                             }
                             showCacheOrError();
                             return;
                           }
                           state_ = State::Fetching;
                         });
}

bool WeatherActivity::resolveCoordinates(const char*& lat, const char*& lon, const char*& city) {
  const uint8_t choice = SETTINGS.weatherCity;
  if (choice >= 1 && choice <= WeatherParser::kCityCount) {
    const WeatherCity& c = WeatherParser::kCities[choice - 1];
    lat = c.lat;
    lon = c.lon;
    city = c.name;
    return true;
  }
  if (detectedLat_[0] == '\0') {
    if (!HttpDownloader::fetchUrl("http://ip-api.com/json/?fields=lat,lon,city", response_) ||
        !WeatherParser::parseIpLocation(response_.data(), response_.size(), detectedLat_, sizeof(detectedLat_),
                                        detectedLon_, sizeof(detectedLon_), detectedCity_, sizeof(detectedCity_))) {
      LOG_ERR("WEATHER", "IP geolocation failed");
      detectedLat_[0] = '\0';
      return false;
    }
    LOG_INF("WEATHER", "IP geolocation: %s (%s, %s)", detectedCity_, detectedLat_, detectedLon_);
  }
  lat = detectedLat_;
  lon = detectedLon_;
  city = detectedCity_[0] != '\0' ? detectedCity_ : tr(STR_WEATHER_CITY_AUTO);
  return true;
}

void WeatherActivity::fetchNow() {
  lastFetchMs_ = millis();
  const char* lat = nullptr;
  const char* lon = nullptr;
  const char* city = nullptr;
  if (!resolveCoordinates(lat, lon, city)) {
    showCacheOrError();
    return;
  }

  char url[384];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,"
           "uv_index,surface_pressure,dew_point_2m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
           "&forecast_days=%d&timezone=auto",
           lat, lon, WEATHER_FORECAST_DAYS + 1);

  WeatherData fresh;
  DailyForecast freshForecast[WEATHER_FORECAST_DAYS];
  int freshCount = 0;
  if (!HttpDownloader::fetchUrl(url, response_) ||
      !WeatherParser::parseForecast(response_.data(), response_.size(), fresh, freshForecast, freshCount)) {
    LOG_ERR("WEATHER", "Forecast fetch failed (%u bytes)", static_cast<unsigned>(response_.size()));
    showCacheOrError();
    return;
  }

  // Best effort: a missing AQI leaves its cell blank rather than failing the fetch.
  snprintf(url, sizeof(url),
           "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=%s&longitude=%s&current=us_aqi", lat, lon);
  int aqi = -1;
  if (HttpDownloader::fetchUrl(url, response_) &&
      WeatherParser::parseAirQuality(response_.data(), response_.size(), aqi)) {
    fresh.airQuality = aqi;
  }

  // "Updated" is shown in the city's local time, which needs no device
  // timezone. Without a synced clock, the API's observation time stands in.
  char updatedAt[sizeof(updatedAt_)];
  if (AppNetwork::clockValid()) {
    const time_t local = time(nullptr) + fresh.utcOffsetSeconds;
    struct tm parts = {};
    gmtime_r(&local, &parts);
    snprintf(updatedAt, sizeof(updatedAt), "%02d:%02d", parts.tm_hour, parts.tm_min);
  } else {
    copyStr(updatedAt, fresh.observed);
  }

  {
    // The render task reads these fields; swap them in under its lock.
    RenderLock lock(*this);
    weather_ = fresh;
    forecastCount_ = freshCount;
    for (int i = 0; i < freshCount; i++) forecast_[i] = freshForecast[i];
    copyStr(cityName_, city);
    copyStr(updatedAt_, updatedAt);
    hasData_ = true;
    showingCache_ = false;
  }
  WEATHER_CACHE.update(weather_, forecast_, forecastCount_, cityName_, updatedAt_);

  // The header reads this live from RAM; it is persisted once on exit.
  // Clamp away from the -128 "no data" sentinel.
  const long rounded = lroundf(weather_.temperature);
  const auto tempC = static_cast<int8_t>(std::clamp<long>(rounded, -127, 127));
  if (tempC != SETTINGS.weatherLastTempC) {
    SETTINGS.weatherLastTempC = tempC;
    topbarDirty_ = true;
  }

  state_ = State::Displaying;
  requestUpdate(true);
}

void WeatherActivity::openCityPicker() {
  std::vector<std::string> labels;
  labels.reserve(CrossPointSettings::APP_WEATHER_CITY_CHOICES);
  char label[WEATHER_CITY_NAME_LEN];
  for (uint8_t i = 0; i < CrossPointSettings::APP_WEATHER_CITY_CHOICES; i++) {
    formatCityChoice(i, label, sizeof(label));
    labels.emplace_back(label);
  }
  cityPopup_.show(tr(STR_WEATHER_CITY), labels, SETTINGS.weatherCity, [this](const int index) {
    if (index == SETTINGS.weatherCity) return;
    SETTINGS.weatherCity = static_cast<uint8_t>(index);
    SETTINGS.saveToFile();
    startRefresh(/*userInitiated=*/true);
  });
  requestUpdate();
}

void WeatherActivity::openOptions() {
  const uint8_t city = SETTINGS.weatherCity;
  const uint8_t orientation = SETTINGS.weatherOrientation;
  auto options = makeUniqueNoThrow<AppOptionsActivity>(renderer, mappedInput, StrId::STR_APP_OPTIONS, kOptionRows,
                                                       kOptionRowCount);
  if (!options) {
    LOG_ERR("WEATHER", "Cannot allocate options screen");
    return;
  }
  startActivityForResult(std::move(options), [this, city, orientation](const ActivityResult&) {
    if (SETTINGS.weatherOrientation != orientation) applyOrientation();
    if (SETTINGS.weatherCity != city) startRefresh(/*userInitiated=*/true);
  });
}

void WeatherActivity::loop() {
  if (cityPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (AppChrome::backTapped(mappedInput, renderer) || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    exitApp();
    return;
  }

  switch (state_) {
    case State::Connecting:
      if (AppNetwork::isConnected()) {
        state_ = State::Fetching;
        requestUpdate(true);
      } else if (millis() - connectStartMs_ >= AppNetwork::kSilentConnectTimeoutMs) {
        LOG_ERR("WEATHER", "Silent Wi-Fi connect timed out");
        AppNetwork::tearDown();
        onConnectFailed(userRequestedFetch_);
      }
      return;
    case State::Fetching:
      // Paint "Fetching..." before blocking on the network.
      requestUpdateAndWait();
      fetchNow();
      return;
    case State::Displaying:
    case State::Error:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startRefresh(/*userInitiated=*/true);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openOptions();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openCityPicker();
    return;
  }
  if (mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Down) {
    startRefresh(/*userInitiated=*/true);
    return;
  }
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    openOptions();
    return;
  }

  const uint32_t intervalMs = AppRefreshInterval::minutesToMs(
      AppRefreshInterval::minutesAt(AppRefreshInterval::kWeatherMinutes, SETTINGS.weatherRefreshInterval));
  if (lastFetchMs_ != 0 && millis() - lastFetchMs_ >= intervalMs) {
    startRefresh(/*userInitiated=*/false);
  }
}

void WeatherActivity::renderStatus(const Rect& area, const char* text) const {
  UITheme::drawCenteredWrappedText(renderer, area, UI_12_FONT_ID, area.y + area.height / 3, text, 3, true,
                                   EpdFontFamily::BOLD);
}

void WeatherActivity::drawStat(const int x, const int y, const int width, const char* label, const char* value) const {
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  renderer.drawText(SMALL_FONT_ID, x + (width - labelWidth) / 2, y, label);
  if (value[0] == '\0') return;
  // Narrow landscape columns fall back to smaller faces before clipping.
  int fontId = UI_12_FONT_ID;
  int valueWidth = renderer.getTextWidth(fontId, value, EpdFontFamily::BOLD);
  for (const int smaller : {UI_10_FONT_ID, SMALL_FONT_ID}) {
    if (valueWidth <= width - 4) break;
    fontId = smaller;
    valueWidth = renderer.getTextWidth(fontId, value, EpdFontFamily::BOLD);
  }
  renderer.drawText(fontId, x + (width - valueWidth) / 2, y + renderer.getLineHeight(SMALL_FONT_ID) + 3, value, true,
                    EpdFontFamily::BOLD);
}

void WeatherActivity::drawForecast(const int x, const int y, const int width) const {
  if (forecastCount_ <= 0) return;
  renderer.drawLine(x, y, x + width, y, true);
  const int top = y + 8;
  const int cellWidth = width / forecastCount_;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  char buf[24];
  for (int i = 0; i < forecastCount_; i++) {
    const DailyForecast& f = forecast_[i];
    const int cx = x + i * cellWidth + cellWidth / 2;
    const char* day = I18N.get(kWeekdayIds[f.weekday % 7]);
    renderer.drawText(SMALL_FONT_ID, cx - renderer.getTextWidth(SMALL_FONT_ID, day) / 2, top, day);
    const freeink::Icon& icon = conditionIcon(f.weatherCode, false);
    drawLucideIcon(renderer, icon, cx - icon.w / 2, top + smallLine + 2);
    const int tempsY = top + smallLine + 2 + icon.h + 4;
    char low[8];
    formatTemp(f.tempMax, buf, sizeof(buf));
    formatTemp(f.tempMin, low, sizeof(low));
    renderer.drawText(SMALL_FONT_ID, cx - renderer.getTextWidth(SMALL_FONT_ID, buf, EpdFontFamily::BOLD) / 2, tempsY,
                      buf, true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, cx - renderer.getTextWidth(SMALL_FONT_ID, low) / 2, tempsY + smallLine, low);
  }
}

void WeatherActivity::drawUpdatedLine(const Rect& area, const int y) const {
  if (updatedAt_[0] == '\0') return;
  char buf[64];
  snprintf(buf, sizeof(buf), showingCache_ ? tr(STR_CACHED_AT) : tr(STR_UPDATED_AT), updatedAt_);
  UITheme::drawCenteredText(renderer, area, SMALL_FONT_ID, y, buf);
}

void WeatherActivity::renderPortrait(const Rect& area) {
  char buf[48];
  int y = area.y;

  UITheme::drawCenteredText(renderer, area, UI_10_FONT_ID, y, conditionLabel(weather_.weatherCode));
  y += renderer.getLineHeight(UI_10_FONT_ID) + 8;

  // Icon and temperature side by side, centered as one cluster.
  const freeink::Icon& icon = conditionIcon(weather_.weatherCode, true);
  formatTemp(weather_.temperature, buf, sizeof(buf));
  const int tempWidth = renderer.getTextWidth(displayFontId_, buf, EpdFontFamily::BOLD);
  const int tempHeight = renderer.getLineHeight(displayFontId_);
  const int clusterWidth = icon.w + 16 + tempWidth;
  const int clusterX = area.x + (area.width - clusterWidth) / 2;
  const int clusterHeight = std::max<int>(icon.h, tempHeight);
  drawLucideIcon(renderer, icon, clusterX, y + (clusterHeight - icon.h) / 2);
  renderer.drawText(displayFontId_, clusterX + icon.w + 16, y + (clusterHeight - tempHeight) / 2, buf, true,
                    EpdFontFamily::BOLD);
  y += clusterHeight + 12;

  // Details card: 2 columns x 4 rows, laid out from the font metrics.
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int valueHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = labelHeight + valueHeight + 13;
  constexpr int kRows = 4;
  constexpr int kPad = 6;
  const int cardHeight = kPad * 2 + rowHeight * kRows;
  const int colWidth = area.width / 2;
  const int contentTop = y + kPad;
  renderer.drawRoundedRect(area.x, y, area.width, cardHeight, 1, 8, true);
  renderer.drawLine(area.x + colWidth, contentTop + 2, area.x + colWidth, contentTop + rowHeight * kRows - 2, true);
  for (int r = 1; r < kRows; r++) {
    renderer.drawLine(area.x + 8, contentTop + r * rowHeight, area.x + area.width - 8, contentTop + r * rowHeight,
                      true);
  }
  const auto cell = [&](const int col, const int row, const char* label, const char* value) {
    drawStat(area.x + col * colWidth, contentTop + row * rowHeight + 3, colWidth, label, value);
  };
  formatTemp(weather_.feelsLike, buf, sizeof(buf));
  cell(0, 0, tr(STR_FEELS_LIKE), buf);
  snprintf(buf, sizeof(buf), "%d%%", weather_.humidity);
  cell(1, 0, tr(STR_HUMIDITY), buf);
  snprintf(buf, sizeof(buf), "%.0f km/h", static_cast<double>(weather_.windSpeed));
  cell(0, 1, tr(STR_WIND), buf);
  snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(weather_.uvIndex));
  cell(1, 1, tr(STR_UV_INDEX), buf);
  if (weather_.airQuality >= 0) {
    snprintf(buf, sizeof(buf), "%d", weather_.airQuality);
  } else {
    buf[0] = '\0';
  }
  cell(0, 2, tr(STR_AIR_QUALITY), buf);
  snprintf(buf, sizeof(buf), "%.0f hPa", static_cast<double>(weather_.pressure));
  cell(1, 2, tr(STR_PRESSURE), buf);
  if (weather_.sunrise[0] != '\0' && weather_.sunset[0] != '\0') {
    snprintf(buf, sizeof(buf), "%s / %s", weather_.sunrise, weather_.sunset);
  } else {
    buf[0] = '\0';
  }
  cell(0, 3, tr(STR_SUNRISE_SUNSET), buf);
  formatTemp(weather_.dewPoint, buf, sizeof(buf));
  cell(1, 3, tr(STR_DEW_POINT), buf);
  y += cardHeight + 12;

  drawForecast(area.x, y, area.width);
  drawUpdatedLine(area, area.y + area.height - renderer.getLineHeight(SMALL_FONT_ID));
}

void WeatherActivity::renderLandscape(const Rect& area) {
  char buf[48];
  int y = area.y;

  // Left: icon, temperature and condition. Right: 2 x 4 stat grid.
  const int leftWidth = area.width / 3;
  const freeink::Icon& icon = conditionIcon(weather_.weatherCode, true);
  drawLucideIcon(renderer, icon, area.x + (leftWidth - icon.w) / 2, y);
  int leftY = y + icon.h + 4;
  formatTemp(weather_.temperature, buf, sizeof(buf));
  const int tempWidth = renderer.getTextWidth(displayFontId_, buf, EpdFontFamily::BOLD);
  renderer.drawText(displayFontId_, area.x + (leftWidth - tempWidth) / 2, leftY, buf, true, EpdFontFamily::BOLD);
  leftY += renderer.getLineHeight(displayFontId_) + 2;
  const Rect leftArea{area.x, area.y, leftWidth, area.height};
  UITheme::drawCenteredText(renderer, leftArea, UI_10_FONT_ID, leftY, conditionLabel(weather_.weatherCode));
  leftY += renderer.getLineHeight(UI_10_FONT_ID);

  const int gridX = area.x + leftWidth;
  const int gridWidth = area.width - leftWidth;
  const int colWidth = gridWidth / 4;
  const int rowHeight = renderer.getLineHeight(SMALL_FONT_ID) + renderer.getLineHeight(UI_12_FONT_ID) + 10;
  const auto stat = [&](const int col, const int row, const char* label, const char* value) {
    drawStat(gridX + col * colWidth, y + row * rowHeight, colWidth, label, value);
  };
  char feels[16];
  formatTemp(weather_.feelsLike, feels, sizeof(feels));
  stat(0, 0, tr(STR_FEELS_LIKE), feels);
  snprintf(buf, sizeof(buf), "%d%%", weather_.humidity);
  stat(1, 0, tr(STR_HUMIDITY), buf);
  snprintf(buf, sizeof(buf), "%.0f km/h", static_cast<double>(weather_.windSpeed));
  stat(2, 0, tr(STR_WIND), buf);
  snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(weather_.uvIndex));
  stat(3, 0, tr(STR_UV_INDEX), buf);
  if (weather_.airQuality >= 0) {
    snprintf(buf, sizeof(buf), "%d", weather_.airQuality);
  } else {
    buf[0] = '\0';
  }
  stat(0, 1, tr(STR_AIR_QUALITY), buf);
  snprintf(buf, sizeof(buf), "%.0f hPa", static_cast<double>(weather_.pressure));
  stat(1, 1, tr(STR_PRESSURE), buf);
  formatTemp(weather_.dewPoint, buf, sizeof(buf));
  stat(2, 1, tr(STR_DEW_POINT), buf);
  if (weather_.sunrise[0] != '\0' && weather_.sunset[0] != '\0') {
    snprintf(buf, sizeof(buf), "%s / %s", weather_.sunrise, weather_.sunset);
  } else {
    buf[0] = '\0';
  }
  stat(3, 1, tr(STR_SUNRISE_SUNSET), buf);

  y = std::max(leftY, y + rowHeight * 2) + 8;
  drawForecast(area.x, y, area.width);
  drawUpdatedLine(area, area.y + area.height - renderer.getLineHeight(SMALL_FONT_ID));
}

void WeatherActivity::render(RenderLock&&) {
  if (cityPopup_.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const char* title = hasData_ && cityName_[0] != '\0' ? cityName_ : tr(STR_WEATHER);
  const Rect area = AppChrome::drawHeader(renderer, mappedInput, title);

  const bool busy = state_ == State::Connecting || state_ == State::Fetching;
  if (busy && !hasData_) {
    renderStatus(area, tr(STR_FETCHING_WEATHER));
  } else if (!hasData_) {
    renderStatus(area, tr(STR_WEATHER_ERROR));
  } else if (renderer.getScreenHeight() < kCompactLayoutMaxHeight) {
    renderLandscape(area);
  } else {
    renderPortrait(area);
  }
  // A refresh over existing data keeps the data on screen and says so in the footer.
  if (busy && hasData_) {
    const int y = area.y + area.height - renderer.getLineHeight(SMALL_FONT_ID);
    renderer.fillRect(area.x, y, area.width, renderer.getLineHeight(SMALL_FONT_ID), false);
    UITheme::drawCenteredText(renderer, area, SMALL_FONT_ID, y, tr(STR_FETCHING_WEATHER));
  }

  const auto hints = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), busy ? "" : tr(STR_REFRESH),
                                           busy ? "" : tr(STR_APP_OPTIONS), busy ? "" : tr(STR_WEATHER_CITY));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4, true);

  HalDisplay::RefreshMode mode = screenTransitionRefresh_.modeFor(static_cast<uint8_t>(hasData_ ? 1 : 0));
  if (forceHalfRefresh_) mode = HalDisplay::HALF_REFRESH;
  forceHalfRefresh_ = false;
  if (mode == HalDisplay::FAST_REFRESH && ++fastRefreshCount_ >= kFullRefreshEvery) mode = HalDisplay::HALF_REFRESH;
  if (mode != HalDisplay::FAST_REFRESH) fastRefreshCount_ = 0;
  renderer.displayBuffer(mode);
}

#endif
