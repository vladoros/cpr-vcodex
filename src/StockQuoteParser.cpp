#include "StockQuoteParser.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cmath>
#include <cstring>

#include "StockFormat.h"

namespace {
void copyStr(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}
}  // namespace

bool StockQuoteParser::parse(const std::string& json, const char* expectedSymbol, StockQuote& out) {
  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("STOCK", "JSON parse error: %s", error.c_str());
    return false;
  }

  JsonObjectConst chart = doc["chart"].as<JsonObjectConst>();
  if (chart.isNull()) return false;

  // Unknown / delisted symbols come back as result: null with error populated.
  JsonArrayConst results = chart["result"].as<JsonArrayConst>();
  if (results.isNull() || results.size() == 0) return false;

  JsonObjectConst meta = results[0]["meta"].as<JsonObjectConst>();
  if (meta.isNull()) return false;

  const float price = meta["regularMarketPrice"] | 0.0f;
  const float previousClose = meta["chartPreviousClose"] | 0.0f;
  const float changePercent = meta["regularMarketChangePercent"] | 0.0f;

  // A zero price means the feed had nothing usable; treat as a failed row so
  // the cache keeps the previous good value and marks it stale.
  if (price <= 0.0f) return false;

  const char* symbol = meta["symbol"] | "";
  if (symbol[0] == '\0') symbol = expectedSymbol;

  copyStr(out.symbol, sizeof(out.symbol), symbol);
  out.price = price;
  out.previousClose = previousClose;
  StockFormat::stockDeriveChange(price, previousClose, changePercent, out.change, out.changePercent);

  copyStr(out.currency, sizeof(out.currency), meta["currency"] | "");
  out.quoteTime = meta["regularMarketTime"] | static_cast<int32_t>(0);
  out.gmtoffset = meta["gmtoffset"] | static_cast<int32_t>(0);
  const char* tz = meta["timezone"] | "";

  JsonObjectConst periods = meta["currentTradingPeriod"].as<JsonObjectConst>();
  if (!periods.isNull()) {
    JsonObjectConst pre = periods["pre"].as<JsonObjectConst>();
    JsonObjectConst regular = periods["regular"].as<JsonObjectConst>();
    JsonObjectConst post = periods["post"].as<JsonObjectConst>();

    if (!pre.isNull()) out.preStart = pre["start"] | static_cast<int32_t>(0);
    if (!regular.isNull()) {
      out.regularStart = regular["start"] | static_cast<int32_t>(0);
      out.regularEnd = regular["end"] | static_cast<int32_t>(0);
    }
    if (!post.isNull()) out.postEnd = post["end"] | static_cast<int32_t>(0);

    if (tz[0] == '\0' && !regular.isNull()) tz = regular["timezone"] | "";
  }

  copyStr(out.tzAbbr, sizeof(out.tzAbbr), tz);
  out.valid = true;
  out.stale = false;
  return true;
}
