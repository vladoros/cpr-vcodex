#include "StockWatchlistStore.h"

#include <Logging.h>
#include <StockWatchlistParser.h>

#include <cstring>

namespace {
// Large S&P 500 constituents plus the index itself. The index keeps the
// optional header value working out of the box, and 16 rows exceed one
// portrait page so pagination is exercised.
constexpr const char* kDefaultSymbols[] = {
    "NVDA",  "AAPL", "MSFT", "AMZN", "META", "GOOGL", "AVGO", "TSLA",
    "BRK-B", "LLY",  "JPM",  "V",    "XOM",  "UNH",   "MA",   "^GSPC",
};
constexpr int kDefaultCount = static_cast<int>(sizeof(kDefaultSymbols) / sizeof(kDefaultSymbols[0]));
static_assert(kDefaultCount <= STOCK_MAX_SYMBOLS);
}  // namespace

StockWatchlistStore::StockWatchlistStore() {
  for (int i = 0; i < kDefaultCount; i++) {
    strncpy(symbols[i], kDefaultSymbols[i], STOCK_SYMBOL_MAX_LEN);
  }
  count = kDefaultCount;
}

void StockWatchlistStore::toJson(JsonDocument& doc) const {
  doc["formatVersion"] = 1;
  JsonArray arr = doc["symbols"].to<JsonArray>();
  for (int i = 0; i < count; i++) arr.add(symbols[i]);
}

bool StockWatchlistStore::fromJson(JsonVariantConst doc) { return applyFromJson(doc); }

bool StockWatchlistStore::applyFromJson(JsonVariantConst doc) {
  StockWatchlistParser::SymbolList accepted = {};
  const int acceptedCount = StockWatchlistParser::collect(doc, accepted);
  if (acceptedCount == 0) {
    LOG_ERR("STOCK", "Watchlist has no valid symbols; keeping the current list");
    return false;
  }
  memcpy(symbols, accepted, sizeof(accepted));
  count = acceptedCount;
  fromFile = true;
  LOG_DBG("STOCK", "Watchlist applied: %d symbols", count);
  return true;
}

bool StockWatchlistStore::saveAtomic() const {
  JsonDocument doc;
  toJson(doc);
  if (!writeDocToFileAtomically(getFilePath(), doc)) {
    LOG_ERR("STOCK", "Failed to save watchlist");
    return false;
  }
  return true;
}
