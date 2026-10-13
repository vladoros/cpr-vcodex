#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include "StockTypes.h"

/**
 * @brief User-owned stock watchlist.
 *
 * Loaded from /.crosspoint/stock_watchlist.json when that file exists and
 * parses; otherwise the compiled default of major S&P 500 constituents is
 * used. The firmware must NEVER overwrite this store on a whim: it is the
 * user's file, normally edited over /api/stocks or by hand.
 *
 * Deliberately separate from StockCacheStore so a firmware cache write can
 * never clobber a hand-edited watchlist.
 */
class StockWatchlistStore : public PersistableStore<StockWatchlistStore> {
 private:
  char symbols[STOCK_MAX_SYMBOLS][STOCK_SYMBOL_MAX_LEN + 1] = {};
  int count = 0;
  bool fromFile = false;

  StockWatchlistStore();
  ~StockWatchlistStore() = default;
  friend class PersistableStore<StockWatchlistStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/stock_watchlist.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Applies an already-parsed symbols array. Normalizes, drops invalid and
  // duplicate entries, caps at STOCK_MAX_SYMBOLS, and falls back to the
  // compiled defaults when nothing valid remains. Returns false only when the
  // payload contained no usable symbol at all (used by the web POST to reply
  // 400); the defaults stay loaded either way.
  bool applyFromJson(JsonVariantConst doc);

  // Persists the current list atomically (temp file + rename) so a power loss
  // during the web POST can never leave a truncated watchlist on the SD card.
  bool saveAtomic() const;

  int getCount() const { return count; }
  const char* getSymbol(int index) const {
    return (index >= 0 && index < count) ? symbols[index] : "";
  }
  bool isFromFile() const { return fromFile; }
};

#define STOCK_WATCHLIST StockWatchlistStore::getInstance()
