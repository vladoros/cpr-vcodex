#include "StockCacheStore.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace {
template <size_t N>
void copyStr(char (&dst)[N], const char* src) {
  strncpy(dst, src ? src : "", N - 1);
  dst[N - 1] = '\0';
}
}  // namespace

bool StockCacheStore::ensureBuffer() {
  if (quotes) return true;
  quotes = makeUniqueNoThrow<StockQuote[]>(STOCK_MAX_SYMBOLS);
  if (!quotes) LOG_ERR("STOCK", "Cannot allocate quote cache");
  return quotes != nullptr;
}

void StockCacheStore::release() {
  quotes.reset();
  quoteCount = 0;
  lastFetchTime = 0;
  loadAttempted_ = false;
}

void StockCacheStore::toJson(JsonDocument& doc) const {
  doc["lastFetchTime"] = lastFetchTime;

  JsonArray arr = doc["quotes"].to<JsonArray>();
  for (int i = 0; i < quoteCount; i++) {
    const StockQuote& q = quotes[i];
    if (!q.valid) continue;
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

  quoteCount = 0;
  if (!ensureBuffer()) return false;
  for (JsonObjectConst obj : doc["quotes"].as<JsonArrayConst>()) {
    if (quoteCount >= STOCK_MAX_SYMBOLS) break;
    const char* symbol = obj["symbol"] | "";
    const float price = obj["price"] | 0.0f;
    if (symbol[0] == '\0' || price <= 0.0f) continue;

    StockQuote& q = quotes[quoteCount++];
    q = StockQuote{};
    copyStr(q.symbol, symbol);
    q.price = price;
    q.previousClose = obj["previousClose"] | 0.0f;
    q.change = obj["change"] | 0.0f;
    q.changePercent = obj["changePercent"] | 0.0f;
    copyStr(q.currency, obj["currency"] | "");
    q.quoteTime = obj["quoteTime"] | static_cast<int32_t>(0);
    q.preStart = obj["preStart"] | static_cast<int32_t>(0);
    q.regularStart = obj["regularStart"] | static_cast<int32_t>(0);
    q.regularEnd = obj["regularEnd"] | static_cast<int32_t>(0);
    q.postEnd = obj["postEnd"] | static_cast<int32_t>(0);
    q.gmtoffset = obj["gmtoffset"] | static_cast<int32_t>(0);
    copyStr(q.tzAbbr, obj["tzAbbr"] | "");
    q.valid = true;
    q.stale = true;
  }

  LOG_DBG("STOCK", "Loaded %d cached quotes", quoteCount);
  return quoteCount > 0;
}

bool StockCacheStore::update(const StockQuote* freshQuotes, const int freshCount, const int32_t freshFetchTime) {
  quoteCount = 0;
  if (!ensureBuffer()) return false;
  for (int i = 0; i < freshCount && quoteCount < STOCK_MAX_SYMBOLS; i++) {
    if (freshQuotes[i].valid) quotes[quoteCount++] = freshQuotes[i];
  }
  lastFetchTime = freshFetchTime;
  return saveToFile();
}
