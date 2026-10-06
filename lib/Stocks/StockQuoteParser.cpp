#include "StockQuoteParser.h"

#include <ArduinoJson.h>

#include <cstring>

#include "StockFormat.h"

namespace {
void copyStr(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (src == nullptr) src = "";
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// The chart response also carries indicator arrays and a validRanges list.
// Only meta is read, so the filter keeps the parsed document to a few hundred
// bytes regardless of what Yahoo adds to the payload.
void buildFilter(JsonDocument& filter) {
  JsonObject meta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  meta["regularMarketPrice"] = true;
  meta["chartPreviousClose"] = true;
  meta["regularMarketChangePercent"] = true;
  meta["symbol"] = true;
  meta["currency"] = true;
  meta["regularMarketTime"] = true;
  meta["gmtoffset"] = true;
  meta["timezone"] = true;
  meta["currentTradingPeriod"] = true;
}
}  // namespace

namespace StockQuoteParser {

bool parse(const char* json, const size_t length, const char* expectedSymbol, StockQuote& out) {
  if (json == nullptr || length == 0) return false;

  JsonDocument filter;
  buildFilter(filter);
  JsonDocument doc;
  if (deserializeJson(doc, json, length, DeserializationOption::Filter(filter))) return false;

  // Unknown / delisted symbols come back as result: null with error populated.
  JsonObjectConst meta = doc["chart"]["result"][0]["meta"].as<JsonObjectConst>();
  if (meta.isNull()) return false;

  const float price = meta["regularMarketPrice"] | 0.0f;
  const float previousClose = meta["chartPreviousClose"] | 0.0f;
  const float changePercent = meta["regularMarketChangePercent"] | 0.0f;

  // A zero price means the feed had nothing usable; treat as a failed row so
  // the cache keeps the previous good value and marks it stale.
  if (price <= 0.0f) return false;

  const char* symbol = meta["symbol"] | "";
  if (symbol[0] == '\0') symbol = expectedSymbol;

  StockQuote quote;
  copyStr(quote.symbol, sizeof(quote.symbol), symbol);
  quote.price = price;
  quote.previousClose = previousClose;
  StockFormat::stockDeriveChange(price, previousClose, changePercent, quote.change, quote.changePercent);

  copyStr(quote.currency, sizeof(quote.currency), meta["currency"] | "");
  quote.quoteTime = meta["regularMarketTime"] | static_cast<int32_t>(0);
  quote.gmtoffset = meta["gmtoffset"] | static_cast<int32_t>(0);
  const char* tz = meta["timezone"] | "";

  JsonObjectConst periods = meta["currentTradingPeriod"].as<JsonObjectConst>();
  if (!periods.isNull()) {
    JsonObjectConst pre = periods["pre"].as<JsonObjectConst>();
    JsonObjectConst regular = periods["regular"].as<JsonObjectConst>();
    JsonObjectConst post = periods["post"].as<JsonObjectConst>();

    quote.preStart = pre["start"] | static_cast<int32_t>(0);
    quote.regularStart = regular["start"] | static_cast<int32_t>(0);
    quote.regularEnd = regular["end"] | static_cast<int32_t>(0);
    quote.postEnd = post["end"] | static_cast<int32_t>(0);
    if (tz[0] == '\0') tz = regular["timezone"] | "";
  }

  copyStr(quote.tzAbbr, sizeof(quote.tzAbbr), tz);
  quote.valid = true;
  quote.stale = false;
  out = quote;
  return true;
}

}  // namespace StockQuoteParser
