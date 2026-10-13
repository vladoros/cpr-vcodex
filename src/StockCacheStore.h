#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include "StockTypes.h"

/**
 * @brief Last successfully fetched quotes, shown when WiFi/the feed is
 * unavailable instead of an error screen.
 *
 * Disposable firmware data: unlike STOCK_WATCHLIST this file may be rewritten
 * at any time. Session epochs are persisted, never the derived market state,
 * so a cached screen re-derives OPEN/CLOSED against the current clock.
 */
class StockCacheStore : public PersistableStore<StockCacheStore> {
 private:
  StockQuote quotes[STOCK_MAX_SYMBOLS] = {};
  int quoteCount = 0;
  int32_t lastFetchTime = 0;
  char lastUpdateTime[8] = "";

  StockCacheStore() = default;

  friend class PersistableStore<StockCacheStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/stock_cache.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Persists a fresh snapshot after a successful refresh.
  bool update(const StockQuote* freshQuotes, int freshCount, int32_t freshFetchTime, const char* freshUpdateTime);

  bool hasCache() const { return quoteCount > 0; }
  const StockQuote* getQuotes() const { return quotes; }
  int getQuoteCount() const { return quoteCount; }
  int32_t getLastFetchTime() const { return lastFetchTime; }
  const char* getLastUpdateTime() const { return lastUpdateTime; }
};

#define STOCK_CACHE StockCacheStore::getInstance()
