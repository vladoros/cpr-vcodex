#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>
#include <StockTypes.h>

#include <memory>

/**
 * Last fetched quotes, shown when Wi-Fi or the feed is unavailable instead of
 * an error screen. Disposable firmware data: unlike STOCK_WATCHLIST this file
 * may be rewritten at any time. Session epochs are persisted, never the derived
 * market state, so a cached screen re-derives OPEN/CLOSED against the clock.
 *
 * The quotes (~1.5 KB) live on the heap only while Stock Ticker uses them;
 * release() frees them so the C3 does not carry them in DRAM for the whole boot.
 */
class StockCacheStore : public PersistableStore<StockCacheStore> {
 private:
  std::unique_ptr<StockQuote[]> quotes;
  int quoteCount = 0;
  int32_t lastFetchTime = 0;

  StockCacheStore() = default;
  bool ensureBuffer();

  friend class PersistableStore<StockCacheStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/stock_cache.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Persists a fresh snapshot after a refresh sweep.
  bool update(const StockQuote* freshQuotes, int freshCount, int32_t freshFetchTime);

  bool hasCache() const { return quoteCount > 0; }
  const StockQuote* getQuotes() const { return quotes.get(); }
  int getQuoteCount() const { return quoteCount; }
  int32_t getLastFetchTime() const { return lastFetchTime; }

  // Frees the quotes; the next ensureLoaded() reads the file again.
  void release();
};

#define STOCK_CACHE StockCacheStore::getInstance()
