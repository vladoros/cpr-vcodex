#pragma once

#include <string>

#include "StockTypes.h"

/**
 * Parses a Yahoo Finance v8 chart response
 * (https://query1.finance.yahoo.com/v8/finance/chart/{SYMBOL}?interval=1d&range=1d)
 * into a StockQuote.
 *
 * Kept separate from StockFormat.h so the pure helpers stay dependency-free
 * for the native test suite; this TU pulls in ArduinoJson and is only compiled
 * into the firmware.
 */
class StockQuoteParser {
 public:
  // expectedSymbol is the symbol that was requested; it is used only when the
  // response does not echo one back. Returns false for a malformed payload or
  // an unknown symbol (chart.result == null).
  static bool parse(const std::string& json, const char* expectedSymbol, StockQuote& out);
};
