#pragma once

// Plain stock data shared between StockTickerActivity (live fetch/render) and
// StockCacheStore (last-known-good snapshot persisted for offline display).
// Kept dependency-free so the store doesn't have to pull in the activity and
// so the parser can be exercised by native unit tests.

constexpr int STOCK_MAX_SYMBOLS = 24;
constexpr int STOCK_SYMBOL_MAX_LEN = 11;  // room for NUL
constexpr int STOCK_CURRENCY_MAX_LEN = 3;
constexpr int STOCK_TZ_ABBR_MAX_LEN = 5;

// Derived exchange session state. Never persisted: the session epochs are
// stored instead and the state is recomputed on every render, so a cached
// screen sitting across the 09:30 open still shows OPEN.
enum StockMarketState {
  STOCK_MARKET_CLOSED = 0,
  STOCK_MARKET_PRE,
  STOCK_MARKET_OPEN,
  STOCK_MARKET_POST,
};

struct StockQuote {
  char symbol[STOCK_SYMBOL_MAX_LEN + 1] = "";
  float price = 0.0f;
  float previousClose = 0.0f;
  // Yahoo reports regularMarketChange as null for most symbols, so the
  // absolute change is always derived from price - previousClose.
  float change = 0.0f;
  float changePercent = 0.0f;
  char currency[STOCK_CURRENCY_MAX_LEN + 1] = "";
  int32_t quoteTime = 0;  // meta.regularMarketTime (epoch seconds)
  // currentTradingPeriod boundaries, epoch seconds. Zero means "unknown".
  int32_t preStart = 0;
  int32_t regularStart = 0;
  int32_t regularEnd = 0;
  int32_t postEnd = 0;
  int32_t gmtoffset = 0;  // exchange offset from UTC, seconds
  char tzAbbr[STOCK_TZ_ABBR_MAX_LEN + 1] = "";  // e.g. "EDT"
  bool valid = false;
  bool stale = false;
};

// Aggregate result of folding every watchlist row into one header line.
struct StockMarketSummary {
  StockMarketState primary = STOCK_MARKET_CLOSED;
  int mixedCount = 0;  // rows disagreeing with the primary state
  int totalCount = 0;  // valid rows considered
};
