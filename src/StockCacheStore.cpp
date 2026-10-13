#include "StockCacheStore.h"

#include <Logging.h>

#include <algorithm>
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

void StockCacheStore::toJson(JsonDocument& doc) const {
  doc["lastFetchTime"] = lastFetchTime;
  doc["lastUpdateTime"] = lastUpdateTime;

  JsonArray arr = doc["quotes"].to<JsonArray>();
  for (int i = 0; i < quoteCount; i++) {
    const StockQuote& q = quotes[i];
    JsonObject obj = arr.add<JsonObject>();
    obj["symbol"] = q.symbol;
    obj["price"] = q.price;
    obj["previousClose"] = q.previousClose;
    obj["change"] = q.change;
    obj["changePercent"] = q.changePercent;
    obj["currency"] = q.currency;
    obj["quoteTime"] = q.quoteTime;
    obj["preStart"] = q.preStart;
    obj["regularStart"] = q.regularStart;
    obj["regularEnd"] = q.regularEnd;
    obj["postEnd"] = q.postEnd;
    obj["gmtoffset"] = q.gmtoffset;
    obj["tzAbbr"] = q.tzAbbr;
  }
}

bool StockCacheStore::fromJson(JsonVariantConst doc) {
  lastFetchTime = doc["lastFetchTime"] | static_cast<int32_t>(0);
  copyStr(lastUpdateTime, sizeof(lastUpdateTime), doc["lastUpdateTime"] | "");

  quoteCount = 0;
  JsonArrayConst arr = doc["quotes"].as<JsonArrayConst>();
  for (JsonObjectConst obj : arr) {
    if (quoteCount >= STOCK_MAX_SYMBOLS) break;
    const char* symbol = obj["symbol"] | "";
    if (symbol[0] == '\0') continue;

    StockQuote& q = quotes[quoteCount];
    memset(&q, 0, sizeof(q));
    copyStr(q.symbol, sizeof(q.symbol), symbol);
    q.price = obj["price"] | 0.0f;
    q.previousClose = obj["previousClose"] | 0.0f;
    q.change = obj["change"] | 0.0f;
    q.changePercent = obj["changePercent"] | 0.0f;
    copyStr(q.currency, sizeof(q.currency), obj["currency"] | "");
    q.quoteTime = obj["quoteTime"] | static_cast<int32_t>(0);
    q.preStart = obj["preStart"] | static_cast<int32_t>(0);
    q.regularStart = obj["regularStart"] | static_cast<int32_t>(0);
    q.regularEnd = obj["regularEnd"] | static_cast<int32_t>(0);
    q.postEnd = obj["postEnd"] | static_cast<int32_t>(0);
    q.gmtoffset = obj["gmtoffset"] | static_cast<int32_t>(0);
    copyStr(q.tzAbbr, sizeof(q.tzAbbr), obj["tzAbbr"] | "");
    q.valid = q.price > 0.0f;
    q.stale = true;
    quoteCount++;
  }

  LOG_DBG("STOCKCACHE", "Loaded cached quotes: %d rows (lastFetch=%lld)", quoteCount,
          static_cast<long long>(lastFetchTime));
  return quoteCount > 0;
}

bool StockCacheStore::update(const StockQuote* freshQuotes, int freshCount, int32_t freshFetchTime,
                             const char* freshUpdateTime) {
  quoteCount = std::min(freshCount, STOCK_MAX_SYMBOLS);
  for (int i = 0; i < quoteCount; i++) quotes[i] = freshQuotes[i];

  lastFetchTime = freshFetchTime;
  copyStr(lastUpdateTime, sizeof(lastUpdateTime), freshUpdateTime);
  return saveToFile();
}
