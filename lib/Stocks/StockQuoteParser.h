#pragma once

#include <cstddef>

#include "StockTypes.h"

/**
 * Parses a Yahoo Finance v8 chart response
 * (https://query1.finance.yahoo.com/v8/finance/chart/{SYMBOL}?interval=1d&range=1d)
 * into a StockQuote.
 */
namespace StockQuoteParser {

// expectedSymbol is the symbol that was requested; it is used only when the
// response does not echo one back. Returns false for a malformed payload, an
// unknown symbol (chart.result == null) or a zero price.
bool parse(const char* json, size_t length, const char* expectedSymbol, StockQuote& out);

}  // namespace StockQuoteParser
