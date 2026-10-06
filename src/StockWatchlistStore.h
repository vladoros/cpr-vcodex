#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>
#include <StockTypes.h>

/**
 * User-owned stock watchlist.
 *
 * Loaded from /.crosspoint/stock_watchlist.json when that file exists and has
 * at least one valid symbol; otherwise compiled defaults are used. The
 * firmware only writes this file on an explicit user edit (/api/stocks), and
 * it is kept apart from StockCacheStore so a cache write can never clobber it.
 */
class StockWatchlistStore : public PersistableStore<StockWatchlistStore> {
 private:
  char symbols[STOCK_MAX_SYMBOLS][STOCK_SYMBOL_MAX_LEN + 1] = {};
  int count = 0;
  bool fromFile = false;

  StockWatchlistStore();
  friend class PersistableStore<StockWatchlistStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/stock_watchlist.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Applies a parsed {"symbols": [...]} document or bare array. Returns false,
  // leaving the current list untouched, when it holds no usable symbol.
  bool applyFromJson(JsonVariantConst doc);

  // Writes through a temp file so a power loss mid-save never truncates the list.
  bool saveAtomic() const;

  int getCount() const { return count; }
  const char* getSymbol(const int index) const { return (index >= 0 && index < count) ? symbols[index] : ""; }
  bool isFromFile() const { return fromFile; }
};

#define STOCK_WATCHLIST StockWatchlistStore::getInstance()
