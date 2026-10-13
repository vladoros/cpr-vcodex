#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "StockFormat.h"

namespace {

StockQuote makeQuote(int32_t preStart, int32_t regularStart, int32_t regularEnd, int32_t postEnd) {
  StockQuote q;
  q.valid = true;
  q.preStart = preStart;
  q.regularStart = regularStart;
  q.regularEnd = regularEnd;
  q.postEnd = postEnd;
  return q;
}

TEST(StockFormatTest, NormalizesSymbol) {
  char out[16];
  ASSERT_TRUE(StockFormat::stockNormalizeSymbol("  nvda ", out, sizeof(out)));
  EXPECT_STREQ("NVDA", out);
  ASSERT_TRUE(StockFormat::stockNormalizeSymbol("brk-b", out, sizeof(out)));
  EXPECT_STREQ("BRK-B", out);
  ASSERT_TRUE(StockFormat::stockNormalizeSymbol("^gspc", out, sizeof(out)));
  EXPECT_STREQ("^GSPC", out);
}

TEST(StockFormatTest, RejectsInvalidSymbols) {
  char out[16];
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("   ", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("AA PL", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("AAPL;DROP", out, sizeof(out)));
  EXPECT_FALSE(StockFormat::stockNormalizeSymbol("ABCDEFGHIJKL", out, sizeof(out)));
}

TEST(StockFormatTest, UrlEncodesCaret) {
  char out[32];
  ASSERT_TRUE(StockFormat::stockUrlEncodeSymbol("^GSPC", out, sizeof(out)));
  EXPECT_STREQ("%5EGSPC", out);
  ASSERT_TRUE(StockFormat::stockUrlEncodeSymbol("BRK-B", out, sizeof(out)));
  EXPECT_STREQ("BRK-B", out);
  ASSERT_TRUE(StockFormat::stockUrlEncodeSymbol("EURUSD=X", out, sizeof(out)));
  EXPECT_STREQ("EURUSD%3DX", out);
}

TEST(StockFormatTest, DerivesMarketStateAcrossSession) {
  const int32_t pre = 1000;
  const int32_t open = 2000;
  const int32_t close = 3000;
  const int32_t post = 4000;
  const StockQuote q = makeQuote(pre, open, close, post);

  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 500));
  EXPECT_EQ(STOCK_MARKET_PRE, StockFormat::stockDeriveMarketState(q, 1500));
  EXPECT_EQ(STOCK_MARKET_OPEN, StockFormat::stockDeriveMarketState(q, 2500));
  EXPECT_EQ(STOCK_MARKET_POST, StockFormat::stockDeriveMarketState(q, 3500));
  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 4500));
}

TEST(StockFormatTest, UnknownSessionIsClosed) {
  StockQuote q;
  q.valid = true;
  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 2500));
}

TEST(StockFormatTest, StaleWindowResolvesClosed) {
  const StockQuote q = makeQuote(1000, 2000, 3000, 4000);
  // A day later the same cached window is far in the past: CLOSED.
  EXPECT_EQ(STOCK_MARKET_CLOSED, StockFormat::stockDeriveMarketState(q, 86400 + 2500));
}

TEST(StockFormatTest, SummaryOfUniformWatchlist) {
  const StockQuote q = makeQuote(1000, 2000, 3000, 4000);
  StockQuote quotes[3] = {q, q, q};
  const StockMarketSummary s = StockFormat::stockComputeSummary(quotes, 3, 2500);
  EXPECT_EQ(STOCK_MARKET_OPEN, s.primary);
  EXPECT_EQ(0, s.mixedCount);
  EXPECT_EQ(3, s.totalCount);
}

TEST(StockFormatTest, SummaryCountsMixedSessions) {
  const StockQuote us = makeQuote(1000, 2000, 3000, 4000);
  const StockQuote foreign = makeQuote(900, 950, 990, 999);  // closed at t=2500
  StockQuote quotes[2] = {us, foreign};
  const StockMarketSummary s = StockFormat::stockComputeSummary(quotes, 2, 2500);
  EXPECT_EQ(STOCK_MARKET_OPEN, s.primary);
  EXPECT_EQ(1, s.mixedCount);
  EXPECT_EQ(2, s.totalCount);
}

TEST(StockFormatTest, FormatsGroupedPrices) {
  char out[32];
  StockFormat::stockFormatNumber(334.39, 2, out, sizeof(out));
  EXPECT_STREQ("334.39", out);
  StockFormat::stockFormatNumber(7676.05, 2, out, sizeof(out));
  EXPECT_STREQ("7,676.05", out);
  StockFormat::stockFormatNumber(1234567.0, 0, out, sizeof(out));
  EXPECT_STREQ("1,234,567", out);
}

TEST(StockFormatTest, DerivesChangeWhenApiOmitsIt) {
  float change = 0.0f;
  float percent = 0.0f;
  // previousClose present, API percent missing: derive both.
  StockFormat::stockDeriveChange(105.0f, 100.0f, 0.0f, change, percent);
  EXPECT_FLOAT_EQ(5.0f, change);
  EXPECT_FLOAT_EQ(5.0f, percent);

  // API percent present: trust it and still compute the absolute change.
  StockFormat::stockDeriveChange(99.0f, 100.0f, -1.25f, change, percent);
  EXPECT_FLOAT_EQ(-1.0f, change);
  EXPECT_FLOAT_EQ(-1.25f, percent);
}

TEST(StockFormatTest, MissingPreviousCloseIsNotTreatedAsPriceChange) {
  float change = 1.0f;
  float percent = 1.0f;
  StockFormat::stockDeriveChange(334.39f, 0.0f, 0.0f, change, percent);
  EXPECT_FLOAT_EQ(0.0f, change);
  EXPECT_FLOAT_EQ(0.0f, percent);
}

TEST(StockFormatTest, FormatsSignedChange) {
  char out[32];
  StockFormat::stockFormatSigned(4.99, 2, out, sizeof(out));
  EXPECT_STREQ("+4.99", out);
  StockFormat::stockFormatSigned(-1.2, 2, out, sizeof(out));
  EXPECT_STREQ("-1.20", out);
  StockFormat::stockFormatPercent(1.52, out, sizeof(out));
  EXPECT_STREQ("+1.52%", out);
  StockFormat::stockFormatPercent(-0.75, out, sizeof(out));
  EXPECT_STREQ("-0.75%", out);
}

}  // namespace
