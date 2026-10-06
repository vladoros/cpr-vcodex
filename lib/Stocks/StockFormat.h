#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "StockTypes.h"

namespace StockFormat {

inline char toUpperAscii(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

inline bool isSpaceAscii(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// A Yahoo symbol is 1..11 chars from [A-Z0-9.\-^=]. The caret (^GSPC) and the
// equals (=X/=F) forms are stripped/uppercased here, never percent-encoded
// here (that is stockUrlEncodeSymbol's job).
inline bool stockIsValidSymbolChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
         c == '^' || c == '=';
}

// Trims surrounding ASCII whitespace, uppercases, validates. Returns false and
// leaves out untouched when the input is empty or contains an invalid char.
inline bool stockNormalizeSymbol(const char* in, char* out, size_t outSize) {
  if (in == nullptr || out == nullptr || outSize == 0) return false;

  while (*in != '\0' && isSpaceAscii(*in)) in++;
  size_t len = strlen(in);
  while (len > 0 && isSpaceAscii(in[len - 1])) len--;
  if (len == 0 || len > STOCK_SYMBOL_MAX_LEN || len + 1 > outSize) return false;

  for (size_t i = 0; i < len; i++) {
    if (!stockIsValidSymbolChar(in[i])) return false;
    out[i] = toUpperAscii(in[i]);
  }
  out[len] = '\0';
  return true;
}

// Percent-encodes characters outside the RFC 3986 unreserved set, which turns
// ^ into %5E and = into %3D. Returns false when out is too small.
inline bool stockUrlEncodeSymbol(const char* in, char* out, size_t outSize) {
  if (in == nullptr || out == nullptr || outSize == 0) return false;

  static const char kHexDigits[] = "0123456789ABCDEF";
  size_t w = 0;
  for (const char* p = in; *p != '\0'; p++) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    if (unreserved) {
      if (w + 1 >= outSize) return false;
      out[w++] = static_cast<char>(c);
    } else {
      if (w + 3 >= outSize) return false;
      out[w++] = '%';
      out[w++] = kHexDigits[(c >> 4) & 0x0F];
      out[w++] = kHexDigits[c & 0x0F];
    }
  }
  out[w] = '\0';
  return true;
}

// Yahoo's chart meta omits or nulls regularMarketChange and sometimes
// regularMarketChangePercent. Derive whichever is missing from price and the
// previous close, and treat a missing previous close as "no change" rather
// than reporting the whole price as the change.
inline void stockDeriveChange(float price, float previousClose, float apiChangePercent, float& outChange,
                              float& outPercent) {
  outChange = previousClose > 0.0f ? (price - previousClose) : 0.0f;
  outPercent = (apiChangePercent != 0.0f) ? apiChangePercent
                                          : (previousClose > 0.0f ? (outChange / previousClose) * 100.0f : 0.0f);
}

// Folds the stored session epochs into a single state. The epochs are epoch
// seconds; a zero window means "unknown" and yields CLOSED. Because the
// comparison is always against `now`, a cache written yesterday resolves to
// CLOSED without any special "too old" branch.
inline StockMarketState stockDeriveMarketState(const StockQuote& q, int32_t now) {
  if (now <= 0 || q.regularStart <= 0 || q.regularEnd <= q.regularStart) return STOCK_MARKET_CLOSED;

  if (q.preStart > 0 && now >= q.preStart && now < q.regularStart) return STOCK_MARKET_PRE;
  if (now >= q.regularStart && now < q.regularEnd) return STOCK_MARKET_OPEN;
  if (q.postEnd > q.regularEnd && now >= q.regularEnd && now < q.postEnd) return STOCK_MARKET_POST;
  return STOCK_MARKET_CLOSED;
}

// Report the first valid row's state, plus how many other valid rows
// disagree, so the header can append a "(N of M closed)" suffix only when the
// watchlist actually spans exchanges with different sessions.
inline StockMarketSummary stockComputeSummary(const StockQuote* quotes, int count, int32_t now) {
  StockMarketSummary summary;
  bool havePrimary = false;
  for (int i = 0; i < count; i++) {
    const StockQuote& q = quotes[i];
    if (!q.valid) continue;
    summary.totalCount++;
    const StockMarketState state = stockDeriveMarketState(q, now);
    if (!havePrimary) {
      summary.primary = state;
      havePrimary = true;
    } else if (state != summary.primary) {
      summary.mixedCount++;
    }
  }
  return summary;
}

// Inserts thousands separators into the integer part. Handles negatives.
inline void stockInsertGrouping(const char* digits, bool negative, char* out, size_t outSize) {
  const size_t len = strlen(digits);
  size_t w = 0;
  if (negative) {
    if (w + 1 < outSize) out[w++] = '-';
  }
  for (size_t i = 0; i < len; i++) {
    if (i > 0 && ((len - i) % 3) == 0) {
      if (w + 1 < outSize) out[w++] = ',';
    }
    if (w + 1 < outSize) out[w++] = digits[i];
  }
  if (w < outSize) out[w] = '\0';
}

// Formats a price with grouping and a fixed number of decimals (0 or 2).
inline void stockFormatNumber(double value, int decimals, char* out, size_t outSize) {
  char raw[48];
  if (decimals <= 0) {
    snprintf(raw, sizeof(raw), "%.0f", value);
  } else {
    snprintf(raw, sizeof(raw), "%.*f", decimals, value);
  }

  const bool negative = raw[0] == '-';
  const char* p = negative ? raw + 1 : raw;
  const char* dot = strchr(p, '.');
  const size_t intLen = dot ? static_cast<size_t>(dot - p) : strlen(p);

  char grouped[64];
  char intPart[32];
  if (intLen >= sizeof(intPart)) {
    strncpy(intPart, p, sizeof(intPart) - 1);
    intPart[sizeof(intPart) - 1] = '\0';
  } else {
    memcpy(intPart, p, intLen);
    intPart[intLen] = '\0';
  }

  char groupedInt[64];
  stockInsertGrouping(intPart, negative, groupedInt, sizeof(groupedInt));

  if (dot) {
    snprintf(grouped, sizeof(grouped), "%s%s", groupedInt, dot);
  } else {
    snprintf(grouped, sizeof(grouped), "%s", groupedInt);
  }

  strncpy(out, grouped, outSize - 1);
  out[outSize - 1] = '\0';
}

// "+4.99" / "-1.20"; zero renders without a sign.
inline void stockFormatSigned(double value, int decimals, char* out, size_t outSize) {
  if (value > 0) {
    char tmp[48];
    stockFormatNumber(value, decimals, tmp, sizeof(tmp));
    snprintf(out, outSize, "+%s", tmp);
  } else {
    stockFormatNumber(value, decimals, out, outSize);
  }
}

inline void stockFormatPercent(double value, char* out, size_t outSize) {
  if (value > 0) {
    char tmp[48];
    stockFormatNumber(value, 2, tmp, sizeof(tmp));
    snprintf(out, outSize, "+%s%%", tmp);
  } else {
    char tmp[48];
    stockFormatNumber(value, 2, tmp, sizeof(tmp));
    snprintf(out, outSize, "%s%%", tmp);
  }
}

}  // namespace StockFormat
