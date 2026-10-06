#pragma once

#include <ArduinoJson.h>

#include "StockTypes.h"

namespace StockWatchlistParser {

using SymbolList = char[STOCK_MAX_SYMBOLS][STOCK_SYMBOL_MAX_LEN + 1];

// Reads doc["symbols"] (or doc itself when it is a bare array), normalizes
// each entry, drops invalid and duplicate symbols and caps the result at
// STOCK_MAX_SYMBOLS. Returns the number of accepted symbols written to out.
int collect(JsonVariantConst doc, SymbolList& out);

}  // namespace StockWatchlistParser
