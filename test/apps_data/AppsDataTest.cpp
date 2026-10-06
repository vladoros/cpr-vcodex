#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "StockFormat.h"
#include "StockQuoteParser.h"
#include "StockWatchlistParser.h"
#include "WeatherParser.h"

namespace {

// Recorded responses from the live APIs (Yahoo chart v8, Open-Meteo, ip-api).
std::string fixture(const char* name) {
  std::ifstream in(std::string(APPS_DATA_FIXTURES) + "/" + name, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool parseQuote(const std::string& json, const char* symbol, StockQuote& out) {
  return StockQuoteParser::parse(json.data(), json.size(), symbol, out);
}

StockQuote makeQuote(int32_t preStart, int32_t regularStart, int32_t regularEnd, int32_t postEnd) {
  StockQuote q;
  q.valid = true;
  q.preStart = preStart;
  q.regularStart = regularStart;
  q.regularEnd = regularEnd;
  q.postEnd = postEnd;
  return q;
}

TEST(StockFormatTest, NormalizesSymbol) {
  char out[16];
  ASSERT_TRUE(StockFormat::stockNormalizeSymbol("  nvda ", out, sizeof(out)));
  EXPECT_STREQ("NVDA", out);
  ASSERT_TRUE(StockFormat::stockNormalizeSymbol("brk-b", out, sizeof(out)));
  EXPECT_STREQ("BRK-B", out);
  ASSERT_TRUE(StockFormat::stockNormalizeSymbol("^gspc", out, sizeof(out)));
  EXPECT_STREQ("^GSPC", out);
}

TEST(StockFormatTest, RejectsInvalidSymbols) {
  char out[16];
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("   ", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("AA PL", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("AAPL;DROP", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("ABCDEFGHIJKL", out, sizeof(out)));
}

TEST(StockFormatTest, UrlEncodesCaret) {
  char out[32];
  ASSERT_TRUE(StockFormat::stockUrlEncodeSymbol("^GSPC", out, sizeof(out)));
  EXPECT_STREQ("%5EGSPC", out);
  ASSERT_TRUE(StockFormat::stockUrlEncodeSymbol("BRK-B", out, sizeof(out)));
  EXPECT_STREQ("BRK-B", out);
  ASSERT_TRUE(StockFormat::stockUrlEncodeSymbol("EURUSD=X", out, sizeof(out)));
  EXPECT_STREQ("EURUSD%3DX", out);
}

TEST(StockFormatTest, DerivesMarketStateAcrossSession) {
  const int32_t pre = 1000;
  const int32_t open = 2000;
  const int32_t close = 3000;
  const int32_t post = 4000;
  const StockQuote q = makeQuote(pre, open, close, post);

  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 500));
  EXPECT_EQ(STOCK_MARKET_PRE, StockFormat::stockDeriveMarketState(q, 1500));
  EXPECT_EQ(STOCK_MARKET_OPEN, StockFormat::stockDeriveMarketState(q, 2500));
  EXPECT_EQ(STOCK_MARKET_POST, StockFormat::stockDeriveMarketState(q, 3500));
  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 4500));
}

TEST(StockFormatTest, UnknownSessionIsClosed) {
  StockQuote q;
  q.valid = true;
  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 2500));
}

TEST(StockFormatTest, StaleWindowResolvesClosed) {
  const StockQuote q = makeQuote(1000, 2000, 3000, 4000);
  // A day later the same cached window is far in the past: CLOSED.
  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 86400 + 2500));
}

TEST(StockFormatTest, SummaryOfUniformWatchlist) {
  const StockQuote q = makeQuote(1000, 2000, 3000, 4000);
  StockQuote quotes[3] = {q, q, q};
  const StockMarketSummary s = StockFormat::stockComputeSummary(quotes, 3, 2500);
  EXPECT_EQ(STOCK_MARKET_OPEN, s.primary);
  EXPECT_EQ(0, s.mixedCount);
  EXPECT_EQ(3, s.totalCount);
}

TEST(StockFormatTest, SummaryCountsMixedSessions) {
  const StockQuote us = makeQuote(1000, 2000, 3000, 4000);
  const StockQuote foreign = makeQuote(900, 950, 990, 999);  // closed at t=2500
  StockQuote quotes[2] = {us, foreign};
  const StockMarketSummary s = StockFormat::stockComputeSummary(quotes, 2, 2500);
  EXPECT_EQ(STOCK_MARKET_OPEN, s.primary);
  EXPECT_EQ(1, s.mixedCount);
  EXPECT_EQ(2, s.totalCount);
}

TEST(StockFormatTest, FormatsGroupedPrices) {
  char out[32];
  StockFormat::stockFormatNumber(334.39, 2, out, sizeof(out));
  EXPECT_STREQ("334.39", out);
  StockFormat::stockFormatNumber(7676.05, 2, out, sizeof(out));
  EXPECT_STREQ("7,676.05", out);
  StockFormat::stockFormatNumber(1234567.0, 0, out, sizeof(out));
  EXPECT_STREQ("1,234,567", out);
}

TEST(StockFormatTest, DerivesChangeWhenApiOmitsIt) {
  float change = 0.0f;
  float percent = 0.0f;
  // previousClose present, API percent missing: derive both.
  StockFormat::stockDeriveChange(105.0f, 100.0f, 0.0f, change, percent);
  EXPECT_FLOAT_EQ(5.0f, change);
  EXPECT_FLOAT_EQ(5.0f, percent);

  // API percent present: trust it and still compute the absolute change.
  StockFormat::stockDeriveChange(99.0f, 100.0f, -1.25f, change, percent);
  EXPECT_FLOAT_EQ(-1.0f, change);
  EXPECT_FLOAT_EQ(-1.25f, percent);
}

TEST(StockFormatTest, MissingPreviousCloseIsNotTreatedAsPriceChange) {
  float change = 1.0f;
  float percent = 1.0f;
  StockFormat::stockDeriveChange(334.39f, 0.0f, 0.0f, change, percent);
  EXPECT_FLOAT_EQ(0.0f, change);
  EXPECT_FLOAT_EQ(0.0f, percent);
}

TEST(StockFormatTest, FormatsSignedChange) {
  char out[32];
  StockFormat::stockFormatSigned(4.99, 2, out, sizeof(out));
  EXPECT_STREQ("+4.99", out);
  StockFormat::stockFormatSigned(-1.2, 2, out, sizeof(out));
  EXPECT_STREQ("-1.20", out);
  StockFormat::stockFormatPercent(1.52, out, sizeof(out));
  EXPECT_STREQ("+1.52%", out);
  StockFormat::stockFormatPercent(-0.75, out, sizeof(out));
  EXPECT_STREQ("-0.75%", out);
}

TEST(StockQuoteParserTest, ParsesEquity) {
  StockQuote q;
  ASSERT_TRUE(parseQuote(fixture("yahoo_aapl.json"), "AAPL", q));
  EXPECT_STREQ("AAPL", q.symbol);
  EXPECT_STREQ("USD", q.currency);
  EXPECT_STREQ("EDT", q.tzAbbr);
  EXPECT_FLOAT_EQ(332.55f, q.price);
  EXPECT_FLOAT_EQ(332.89f, q.previousClose);
  EXPECT_NEAR(-0.34f, q.change, 0.001f);
  EXPECT_FLOAT_EQ(-0.102f, q.changePercent);
  EXPECT_EQ(1791309641, q.quoteTime);
  EXPECT_EQ(-14400, q.gmtoffset);
  EXPECT_EQ(1791273600, q.preStart);
  EXPECT_EQ(1791293400, q.regularStart);
  EXPECT_EQ(1791316800, q.regularEnd);
  EXPECT_EQ(1791331200, q.postEnd);
  EXPECT_TRUE(q.valid);
  EXPECT_FALSE(q.stale);
  EXPECT_EQ(STOCK_MARKET_OPEN, StockFormat::stockDeriveMarketState(q, q.quoteTime));
}

TEST(StockQuoteParserTest, ParsesIndexAndForeignExchange) {
  StockQuote index;
  ASSERT_TRUE(parseQuote(fixture("yahoo_gspc.json"), "^GSPC", index));
  EXPECT_STREQ("^GSPC", index.symbol);
  EXPECT_GT(index.price, 1000.0f);

  StockQuote toyota;
  ASSERT_TRUE(parseQuote(fixture("yahoo_7203t.json"), "7203.T", toyota));
  EXPECT_STREQ("JPY", toyota.currency);
  EXPECT_STREQ("JST", toyota.tzAbbr);
  EXPECT_EQ(32400, toyota.gmtoffset);
}

TEST(StockQuoteParserTest, RejectsUnknownSymbolAndGarbage) {
  StockQuote q;
  q.price = 12.0f;
  EXPECT_FALSE(parseQuote(fixture("yahoo_not_found.json"), "NOTAREALSYM", q));
  EXPECT_FALSE(parseQuote("not json", "AAPL", q));
  EXPECT_FALSE(parseQuote("", "AAPL", q));
  EXPECT_FALSE(parseQuote(R"({"chart":{"result":[{"meta":{"regularMarketPrice":0}}]}})", "AAPL", q));
  // A failed parse must leave the previous row untouched so it can be shown stale.
  EXPECT_FLOAT_EQ(12.0f, q.price);
}

TEST(StockQuoteParserTest, FallsBackToRequestedSymbol) {
  StockQuote q;
  ASSERT_TRUE(
      parseQuote(R"({"chart":{"result":[{"meta":{"regularMarketPrice":10.5,"chartPreviousClose":10}}]}})", "XYZ", q));
  EXPECT_STREQ("XYZ", q.symbol);
  EXPECT_FLOAT_EQ(0.5f, q.change);
  EXPECT_FLOAT_EQ(5.0f, q.changePercent);
}

TEST(StockWatchlistParserTest, NormalizesDedupesAndCaps) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"formatVersion":1,"symbols":[" aapl","AAPL","bad sym","^gspc",42,"brk-b"]})"));
  StockWatchlistParser::SymbolList out = {};
  ASSERT_EQ(3, StockWatchlistParser::collect(doc.as<JsonVariantConst>(), out));
  EXPECT_STREQ("AAPL", out[0]);
  EXPECT_STREQ("^GSPC", out[1]);
  EXPECT_STREQ("BRK-B", out[2]);

  JsonDocument many;
  JsonArray arr = many.to<JsonArray>();
  for (int i = 0; i < STOCK_MAX_SYMBOLS + 5; i++) arr.add("S" + std::to_string(i));
  EXPECT_EQ(STOCK_MAX_SYMBOLS, StockWatchlistParser::collect(many.as<JsonVariantConst>(), out));
}

TEST(StockWatchlistParserTest, EmptyOrMissingListYieldsNothing) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"symbols":[]})"));
  StockWatchlistParser::SymbolList out = {};
  EXPECT_EQ(0, StockWatchlistParser::collect(doc.as<JsonVariantConst>(), out));
  ASSERT_FALSE(deserializeJson(doc, R"({"other":1})"));
  EXPECT_EQ(0, StockWatchlistParser::collect(doc.as<JsonVariantConst>(), out));
}

TEST(WeatherParserTest, ParsesForecast) {
  const std::string json = fixture("openmeteo_forecast.json");
  WeatherData weather;
  DailyForecast forecast[WEATHER_FORECAST_DAYS];
  int count = 0;
  ASSERT_TRUE(WeatherParser::parseForecast(json.data(), json.size(), weather, forecast, count));
  EXPECT_FLOAT_EQ(18.5f, weather.temperature);
  EXPECT_FLOAT_EQ(16.2f, weather.feelsLike);
  EXPECT_EQ(31, weather.humidity);
  EXPECT_EQ(0, weather.weatherCode);
  EXPECT_FLOAT_EQ(1011.9f, weather.pressure);
  EXPECT_EQ(10800, weather.utcOffsetSeconds);
  EXPECT_STREQ("21:00", weather.observed);
  EXPECT_STREQ("07:19", weather.sunrise);
  EXPECT_STREQ("18:47", weather.sunset);
  EXPECT_EQ(-1, weather.airQuality);

  ASSERT_EQ(WEATHER_FORECAST_DAYS, count);
  EXPECT_EQ(3, forecast[0].weekday);  // 2026-10-07 is a Wednesday
  EXPECT_FLOAT_EQ(26.5f, forecast[0].tempMax);
  EXPECT_FLOAT_EQ(8.1f, forecast[0].tempMin);
  EXPECT_EQ(80, forecast[3].weatherCode);
  EXPECT_EQ(95, forecast[4].weatherCode);
  EXPECT_EQ(0, forecast[4].weekday);  // 2026-10-11 is a Sunday
}

TEST(WeatherParserTest, RejectsMissingCurrentBlock) {
  WeatherData weather;
  DailyForecast forecast[WEATHER_FORECAST_DAYS];
  int count = 7;
  const std::string json = R"({"daily":{"time":["2026-10-06"]}})";
  EXPECT_FALSE(WeatherParser::parseForecast(json.data(), json.size(), weather, forecast, count));
  EXPECT_EQ(0, count);
  EXPECT_FALSE(WeatherParser::parseForecast("{", 1, weather, forecast, count));
}

TEST(WeatherParserTest, ParsesAirQualityAndLocation) {
  const std::string aqiJson = fixture("openmeteo_aqi.json");
  int aqi = -1;
  ASSERT_TRUE(WeatherParser::parseAirQuality(aqiJson.data(), aqiJson.size(), aqi));
  EXPECT_EQ(85, aqi);
  const std::string missing = R"({"current":{"us_aqi":null}})";
  EXPECT_FALSE(WeatherParser::parseAirQuality(missing.data(), missing.size(), aqi));

  const std::string ipJson = fixture("ipapi.json");
  char lat[16];
  char lon[16];
  char city[32];
  ASSERT_TRUE(WeatherParser::parseIpLocation(ipJson.data(), ipJson.size(), lat, sizeof(lat), lon, sizeof(lon), city,
                                             sizeof(city)));
  EXPECT_STREQ("45.7537", lat);
  EXPECT_STREQ("21.2257", lon);
  EXPECT_STREQ("Timi\xc8\x99oara", city);
}

TEST(WeatherParserTest, MapsWmoCodes) {
  using WeatherParser::conditionForCode;
  EXPECT_EQ(WeatherCondition::Clear, conditionForCode(0));
  EXPECT_EQ(WeatherCondition::PartlyCloudy, conditionForCode(2));
  EXPECT_EQ(WeatherCondition::Fog, conditionForCode(45));
  EXPECT_EQ(WeatherCondition::Drizzle, conditionForCode(53));
  EXPECT_EQ(WeatherCondition::Rain, conditionForCode(63));
  EXPECT_EQ(WeatherCondition::Snow, conditionForCode(73));
  EXPECT_EQ(WeatherCondition::Showers, conditionForCode(81));
  EXPECT_EQ(WeatherCondition::Snow, conditionForCode(86));
  EXPECT_EQ(WeatherCondition::Thunderstorm, conditionForCode(95));
}

TEST(WeatherParserTest, ComputesWeekdays) {
  uint8_t day = 9;
  ASSERT_TRUE(WeatherParser::weekdayFromIsoDate("2026-10-06", day));
  EXPECT_EQ(2, day);  // Tuesday
  ASSERT_TRUE(WeatherParser::weekdayFromIsoDate("2024-02-29", day));
  EXPECT_EQ(4, day);  // Thursday
  ASSERT_TRUE(WeatherParser::weekdayFromIsoDate("2000-01-01", day));
  EXPECT_EQ(6, day);  // Saturday
  EXPECT_FALSE(WeatherParser::weekdayFromIsoDate("bogus", day));
  EXPECT_FALSE(WeatherParser::weekdayFromIsoDate("2026-13-01", day));
}

}  // namespace
