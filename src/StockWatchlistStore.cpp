#include "StockWatchlistStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "StockFormat.h"

namespace {
// 15 of the largest S&P 500 constituents plus the S&P 500 index. The index is
// included deliberately: it exercises the index symbol form (^GSPC), keeps the
// optional top-bar value working out of the box, and pushes the list past the
// portrait page size so pagination is actually exercised.
const char* const DEFAULT_SYMBOLS[] = {
    "NVDA", "AAPL", "MSFT", "AMZN",  "META",  "GOOGL", "AVGO", "TSLA",
    "BRK-B", "LLY",  "JPM",   "V",     "XOM",   "UNH",   "MA",   "^GSPC",
};
constexpr int DEFAULT_COUNT = static_cast<int>(sizeof(DEFAULT_SYMBOLS) / sizeof(DEFAULT_SYMBOLS[0]));
}  // namespace

StockWatchlistStore::StockWatchlistStore() {
  for (int i = 0; i < DEFAULT_COUNT && i < STOCK_MAX_SYMBOLS; i++) {
    strncpy(symbols[i], DEFAULT_SYMBOLS[i], STOCK_SYMBOL_MAX_LEN);
    symbols[i][STOCK_SYMBOL_MAX_LEN] = '\0';
  }
  count = (DEFAULT_COUNT < STOCK_MAX_SYMBOLS) ? DEFAULT_COUNT : STOCK_MAX_SYMBOLS;
  fromFile = false;
}

void StockWatchlistStore::toJson(JsonDocument& doc) const {
  doc["formatVersion"] = 1;
  JsonArray arr = doc["symbols"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    arr.add(symbols[i]);
  }
}

bool StockWatchlistStore::fromJson(JsonVariantConst doc) { return applyFromJson(doc); }

bool StockWatchlistStore::applyFromJson(JsonVariantConst doc) {
  JsonArrayConst arr = doc["symbols"].as<JsonArrayConst>();
  if (arr.isNull() || arr.size() == 0) {
    LOG_DBG("STOCK", "Watchlist file has no symbols; keeping compiled defaults");
    return false;
  }

  char accepted[STOCK_MAX_SYMBOLS][STOCK_SYMBOL_MAX_LEN + 1] = {};
  int acceptedCount = 0;

  for (JsonVariantConst entry : arr) {
    if (acceptedCount >= STOCK_MAX_SYMBOLS) break;
    const char* raw = entry.as<const char*>();
    if (raw == nullptr) continue;

    char normalized[STOCK_SYMBOL_MAX_LEN + 1];
    if (!StockFormat::stockNormalizeSymbol(raw, normalized, sizeof(normalized))) continue;

    bool duplicate = false;
    for (int i = 0; i < acceptedCount; i++) {
      if (strcmp(accepted[i], normalized) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    strncpy(accepted[acceptedCount], normalized, STOCK_SYMBOL_MAX_LEN);
    accepted[acceptedCount][STOCK_SYMBOL_MAX_LEN] = '\0';
    acceptedCount++;
  }

  if (acceptedCount == 0) {
    LOG_DBG("STOCK", "Watchlist contained no valid symbols; keeping compiled defaults");
    return false;
  }

  memcpy(symbols, accepted, sizeof(accepted));
  count = acceptedCount;
  fromFile = true;
  LOG_DBG("STOCK", "Watchlist applied: %d symbols from %s", count, getFilePath());
  return true;
}

bool StockWatchlistStore::saveAtomic() const {
  JsonDocument doc;
  toJson(doc);
  String json;
  serializeJson(doc, json);

  Storage.mkdir("/.crosspoint");
  static const char* kTempPath = "/.crosspoint/stock_watchlist.json.tmp";
  Storage.remove(kTempPath);
  if (!Storage.writeFile(kTempPath, json)) {
    LOG_ERR("STOCK", "Failed to write %s", kTempPath);
    Storage.remove(kTempPath);
    return false;
  }
  Storage.remove(getFilePath());
  if (!Storage.rename(kTempPath, getFilePath())) {
    LOG_ERR("STOCK", "Failed to rename %s into place", kTempPath);
    Storage.remove(kTempPath);
    return false;
  }
  LOG_DBG("STOCK", "Watchlist saved atomically: %d symbols", count);
  return true;
}
