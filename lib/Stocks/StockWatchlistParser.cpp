#include "StockWatchlistParser.h"

#include <cstring>

#include "StockFormat.h"

namespace StockWatchlistParser {

int collect(const JsonVariantConst doc, SymbolList& out) {
  JsonArrayConst arr = doc.is<JsonArrayConst>() ? doc.as<JsonArrayConst>() : doc["symbols"].as<JsonArrayConst>();
  int count = 0;
  for (JsonVariantConst entry : arr) {
    if (count >= STOCK_MAX_SYMBOLS) break;
    const char* raw = entry.as<const char*>();
    char normalized[STOCK_SYMBOL_MAX_LEN + 1];
    if (raw == nullptr || !StockFormat::stockNormalizeSymbol(raw, normalized, sizeof(normalized))) continue;

    bool duplicate = false;
    for (int i = 0; i < count && !duplicate; i++) duplicate = strcmp(out[i], normalized) == 0;
    if (duplicate) continue;

    memcpy(out[count], normalized, sizeof(normalized));
    count++;
  }
  return count;
}

}  // namespace StockWatchlistParser
