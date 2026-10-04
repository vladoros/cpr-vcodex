#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BuddyPet.h"
#include "BuddyProtocol.h"

using namespace claude_buddy;

namespace {

struct Collector {
  std::vector<std::string> lines;
  static void onLine(void* ctx, const char* line, size_t len) {
    static_cast<Collector*>(ctx)->lines.emplace_back(line, len);
  }
};

void feed(LineFramer& framer, Collector& c, const std::string& s) {
  framer.feed(reinterpret_cast<const uint8_t*>(s.data()), s.size(), &Collector::onLine, &c);
}

Events handle(Protocol& p, const std::string& line, uint32_t nowMs = 1000) {
  Events ev;
  p.handleLine(line.c_str(), line.size(), nowMs, ev);
  return ev;
}

const char* kHeartbeatWithPrompt =
    R"({"total":3,"running":1,"waiting":1,"msg":"approve: Bash","entries":["10:42 git push","10:41 yarn test"],)"
    R"("tokens":184502,"tokens_today":31200,"prompt":{"id":"req_abc123","tool":"Bash","hint":"rm -rf /tmp/foo"}})";

}  // namespace

TEST(LineFramer, ReassemblesFragmentsAndSplitsLines) {
  auto framer = std::make_unique<LineFramer>();
  Collector c;
  feed(*framer, c, "{\"a\":");
  feed(*framer, c, "1}\n{\"b\":2}\r\n\n{\"c\"");
  ASSERT_EQ(c.lines.size(), 2u);
  EXPECT_EQ(c.lines[0], "{\"a\":1}");
  EXPECT_EQ(c.lines[1], "{\"b\":2}");
  feed(*framer, c, ":3}\n");
  ASSERT_EQ(c.lines.size(), 3u);
  EXPECT_EQ(c.lines[2], "{\"c\":3}");
}

TEST(LineFramer, DropsOversizeLineAndRecovers) {
  auto framer = std::make_unique<LineFramer>();
  Collector c;
  feed(*framer, c, "{\"evt\":\"" + std::string(kMaxLine * 3, 'x') + "\"}\n{\"ok\":1}\n");
  ASSERT_EQ(c.lines.size(), 1u);
  EXPECT_EQ(c.lines[0], "{\"ok\":1}");
}

TEST(Protocol, ParsesHeartbeatWithPrompt) {
  Protocol p;
  const Events ev = handle(p, kHeartbeatWithPrompt);
  EXPECT_TRUE(ev.heartbeat);
  EXPECT_TRUE(ev.newPrompt);
  const State& s = p.state();
  EXPECT_EQ(s.total, 3);
  EXPECT_EQ(s.running, 1);
  EXPECT_EQ(s.waiting, 1);
  EXPECT_STREQ(s.msg, "approve: Bash");
  ASSERT_EQ(s.entryCount, 2);
  EXPECT_STREQ(s.entries[0], "10:42 git push");
  EXPECT_EQ(s.tokens, 184502u);
  EXPECT_EQ(s.tokensToday, 31200u);
  ASSERT_TRUE(s.hasPrompt);
  EXPECT_STREQ(s.prompt.id, "req_abc123");
  EXPECT_STREQ(s.prompt.tool, "Bash");
  EXPECT_STREQ(s.prompt.hint, "rm -rf /tmp/foo");
  EXPECT_EQ(s.promptSinceMs, 1000u);
}

TEST(Protocol, SamePromptIsNotNewAndKeepsTimestamp) {
  Protocol p;
  handle(p, kHeartbeatWithPrompt, 1000);
  const Events ev = handle(p, kHeartbeatWithPrompt, 5000);
  EXPECT_FALSE(ev.newPrompt);
  EXPECT_EQ(p.state().promptSinceMs, 1000u);
}

TEST(Protocol, TurnEventDoesNotClearPrompt) {
  Protocol p;
  handle(p, kHeartbeatWithPrompt);
  const Events ev = handle(p, R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":"hi"}]})", 2000);
  EXPECT_FALSE(ev.heartbeat);
  EXPECT_FALSE(ev.promptCleared);
  EXPECT_TRUE(p.state().hasPrompt);
  EXPECT_EQ(p.state().lastRxMs, 2000u);
  EXPECT_STREQ(p.state().reply, "hi");
}

TEST(Protocol, HeartbeatWithoutPromptClearsIt) {
  Protocol p;
  handle(p, kHeartbeatWithPrompt);
  const Events ev = handle(p, R"({"total":1,"running":0,"waiting":0,"msg":"idle","entries":[]})");
  EXPECT_TRUE(ev.promptCleared);
  EXPECT_FALSE(p.state().hasPrompt);
  EXPECT_EQ(p.state().entryCount, 0);
}

TEST(Protocol, TruncatesLongFieldsOnUtf8Boundary) {
  Protocol p;
  std::string msg;
  for (int i = 0; i < 60; ++i) msg += "\xC3\xA9";
  handle(p, R"({"total":1,"msg":")" + msg + R"("})");
  const char* m = p.state().msg;
  EXPECT_LT(strlen(m), sizeof(p.state().msg));
  EXPECT_EQ(strlen(m) % 2, 0u);
}

TEST(Protocol, CapsEntries) {
  Protocol p;
  std::string entries = "[";
  for (int i = 0; i < 12; ++i) entries += std::string(i ? "," : "") + "\"e" + std::to_string(i) + "\"";
  entries += "]";
  handle(p, R"({"total":1,"entries":)" + entries + "}");
  EXPECT_EQ(p.state().entryCount, kMaxEntries);
}

TEST(Protocol, IgnoresInvalidJson) {
  Protocol p;
  const Events ev = handle(p, "{not json");
  EXPECT_FALSE(ev.heartbeat);
  EXPECT_FALSE(p.state().everReceived);
}

TEST(Protocol, CommandsAck) {
  Protocol p;
  Events ev = handle(p, R"({"cmd":"owner","name":"Fe\"lix\\"})");
  EXPECT_TRUE(ev.ownerChanged);
  EXPECT_STREQ(p.state().owner, "Felix");
  EXPECT_STREQ(ev.ack, R"({"ack":"owner","ok":true,"n":0})");

  ev = handle(p, R"({"cmd":"name","name":"Clawd"})");
  EXPECT_TRUE(ev.nameChanged);
  EXPECT_STREQ(p.state().name, "Clawd");

  ev = handle(p, R"({"cmd":"unpair"})");
  EXPECT_TRUE(ev.unpair);
  EXPECT_STREQ(ev.ack, R"({"ack":"unpair","ok":true,"n":0})");

  ev = handle(p, R"({"cmd":"status"})");
  EXPECT_TRUE(ev.statusRequested);
  EXPECT_EQ(ev.ack[0], '\0');
}

TEST(Protocol, RejectsTransferAndUnknownCommands) {
  Protocol p;
  Events ev = handle(p, R"({"cmd":"char_begin","name":"bufo","total":184320})");
  EXPECT_STREQ(ev.ack, R"({"ack":"char_begin","ok":false,"n":0,"error":"unsupported"})");
  ev = handle(p, R"({"cmd":"frobnicate"})");
  EXPECT_STREQ(ev.ack, R"({"ack":"frobnicate","ok":false,"n":0,"error":"unknown"})");
}

TEST(Protocol, TimeSync) {
  Protocol p;
  const Events ev = handle(p, R"({"time":[1775731234,-25200]})");
  EXPECT_TRUE(ev.timeSync);
  EXPECT_EQ(ev.epoch, 1775731234);
  EXPECT_EQ(ev.tzOffsetSec, -25200);
  EXPECT_FALSE(ev.heartbeat);
}

TEST(Protocol, CompletedFlag) {
  Protocol p;
  EXPECT_TRUE(handle(p, R"({"total":1,"completed":true})").completed);
}

TEST(Builders, StatusAck) {
  StatusInfo info{"Clawd", "Felix", true, 87, true, 8412, 84200, 42, 3, 8, 5};
  char out[256];
  ASSERT_TRUE(buildStatusAck(info, out, sizeof(out)));
  EXPECT_STREQ(out, R"({"ack":"status","ok":true,"n":0,"data":{"name":"Clawd","owner":"Felix","sec":true,)"
                    R"("bat":{"pct":87,"usb":true},"sys":{"up":8412,"heap":84200},)"
                    R"("stats":{"appr":42,"deny":3,"vel":8,"lvl":5}}})");
  char tiny[16];
  EXPECT_FALSE(buildStatusAck(info, tiny, sizeof(tiny)));
}

TEST(Builders, Permission) {
  char out[128];
  ASSERT_TRUE(buildPermission("req_abc123", true, out, sizeof(out)));
  EXPECT_STREQ(out, R"({"cmd":"permission","id":"req_abc123","decision":"once"})");
  ASSERT_TRUE(buildPermission("a\"b", false, out, sizeof(out)));
  EXPECT_STREQ(out, R"({"cmd":"permission","id":"a\"b","decision":"deny"})");
}

TEST(Mood, Derivation) {
  Protocol p;
  MoodTimers timers;
  EXPECT_EQ(deriveMood(p.state(), false, timers, 0), Mood::Sleep);

  handle(p, R"({"total":1,"running":0,"waiting":0})", 1000);
  EXPECT_TRUE(isLinkLive(p.state(), true, 1000 + kLinkTimeoutMs - 1));
  EXPECT_FALSE(isLinkLive(p.state(), true, 1000 + kLinkTimeoutMs));
  EXPECT_FALSE(isLinkLive(p.state(), false, 1000));
  EXPECT_EQ(deriveMood(p.state(), true, timers, 1000), Mood::Idle);

  handle(p, R"({"total":1,"running":2,"waiting":0})", 1000);
  EXPECT_EQ(deriveMood(p.state(), true, timers, 1000), Mood::Busy);

  timers.celebrateUntilMs = 4000;
  EXPECT_EQ(deriveMood(p.state(), true, timers, 2000), Mood::Celebrate);
  timers.heartUntilMs = 3000;
  EXPECT_EQ(deriveMood(p.state(), true, timers, 2000), Mood::Heart);
  EXPECT_EQ(deriveMood(p.state(), true, timers, 5000), Mood::Busy);

  handle(p, kHeartbeatWithPrompt, 5000);
  EXPECT_EQ(deriveMood(p.state(), true, timers, 2000), Mood::Attention);
}

TEST(Trackers, TokenLevelUp) {
  TokenTracker t(49000);
  EXPECT_FALSE(t.update(100000));
  EXPECT_FALSE(t.update(100500));
  EXPECT_EQ(t.credited(), 49500u);
  EXPECT_TRUE(t.update(101000));
  EXPECT_EQ(t.level(), 1u);
  EXPECT_FALSE(t.update(10));
  EXPECT_EQ(t.credited(), 50000u);
  EXPECT_FALSE(t.update(20));
  EXPECT_EQ(t.credited(), 50010u);
}

TEST(Trackers, VelocityMedian) {
  VelocityTracker v;
  EXPECT_EQ(v.median(), 0u);
  for (uint32_t s : {9u, 1u, 5u}) v.add(s);
  EXPECT_EQ(v.median(), 5u);
  for (int i = 0; i < 10; ++i) v.add(2);
  EXPECT_EQ(v.count(), VelocityTracker::kSamples);
  EXPECT_EQ(v.median(), 2u);
}

TEST(Pet, EverySpeciesAndMoodHasFixedWidthFrame) {
  ASSERT_GT(kPetSpeciesCount, 1);
  for (int sp = 0; sp < kPetSpeciesCount; ++sp) {
    for (Mood m : {Mood::Sleep, Mood::Idle, Mood::Busy, Mood::Attention, Mood::Celebrate, Mood::Heart}) {
      const char* const* frame = petFrame(sp, m);
      for (int r = 0; r < kPetRows; ++r) {
        ASSERT_NE(frame[r], nullptr);
        EXPECT_EQ(strlen(frame[r]), static_cast<size_t>(kPetCols)) << "species " << sp << " row " << r;
      }
    }
  }
}

TEST(Pet, OutOfRangeSpeciesFallsBackToFirst) {
  EXPECT_EQ(petFrame(-1, Mood::Idle), petFrame(0, Mood::Idle));
  EXPECT_EQ(petFrame(kPetSpeciesCount, Mood::Idle), petFrame(0, Mood::Idle));
}

TEST(Reply, StoresAssistantText) {
  Protocol p;
  const Events ev = handle(p, R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":"Hello there"}]})");
  EXPECT_TRUE(ev.replyUpdated);
  EXPECT_FALSE(ev.heartbeat);
  EXPECT_STREQ(p.state().reply, "Hello there");
  EXPECT_EQ(p.state().replyVersion, 1u);
}

TEST(Reply, JoinsTextBlocksAndSkipsOtherBlocks) {
  Protocol p;
  handle(p,
         R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":"First"},)"
         R"({"type":"tool_use","id":"t1","name":"Bash","input":{"command":"ls"}},{"type":"text","text":"Second"}]})");
  EXPECT_STREQ(p.state().reply, "First\n\nSecond");
}

TEST(Reply, IgnoresNonAssistantAndEmptyTurns) {
  Protocol p;
  handle(p, R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":"keep me"}]})");
  Events ev = handle(p, R"({"evt":"turn","role":"user","content":[{"type":"text","text":"question"}]})");
  EXPECT_FALSE(ev.replyUpdated);
  ev = handle(p, R"({"evt":"turn","role":"assistant","content":[{"type":"tool_use","name":"Bash"}]})");
  EXPECT_FALSE(ev.replyUpdated);
  EXPECT_STREQ(p.state().reply, "keep me");
  EXPECT_EQ(p.state().replyVersion, 1u);
}

TEST(Reply, KeepsNewlinesAndDropsOtherControlCharacters) {
  Protocol p;
  handle(p, R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":"a\nb\r\nc\td"}]})");
  EXPECT_STREQ(p.state().reply, "a\nb\nc d");
}

TEST(Reply, TruncatesOnUtf8BoundaryAndDoesNotOverflow) {
  Protocol p;
  std::string text;
  for (size_t i = 0; i < 2200; ++i) text += "\xC3\xA9";
  const std::string line = R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":")" + text + R"("}]})";
  ASSERT_LT(line.size(), kMaxLine) << "test line should fit the framer";
  handle(p, line.substr(0, line.size()));
  const size_t n = strlen(p.state().reply);
  EXPECT_LT(n, kReplyMax);
  EXPECT_EQ(n % 2, 0u);
}

TEST(Reply, FourKilobyteTurnEventSurvivesFraming) {
  auto framer = std::make_unique<LineFramer>();
  Collector c;
  std::string text(3900, 'x');
  feed(*framer, c, R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":")" + text + "\"}]}\n");
  ASSERT_EQ(c.lines.size(), 1u);
  Protocol p;
  p.handleLine(c.lines[0].c_str(), c.lines[0].size(), 0, *std::make_unique<Events>());
  EXPECT_EQ(strlen(p.state().reply), 3900u);
}

TEST(FrameChunk, ReassemblesAtEveryBoundaryWithoutOverrun) {
  for (const size_t maxChunk : {size_t{1}, size_t{2}, size_t{20}, size_t{180}}) {
    for (const size_t len : {size_t{0}, size_t{1}, size_t{19}, size_t{20}, size_t{21}, size_t{179}, size_t{180},
                             size_t{181}, size_t{359}, size_t{360}, size_t{361}, size_t{539}, size_t{540}}) {
      const std::string line(len, 'x');
      std::string joined;
      std::vector<uint8_t> buf(maxChunk + 8, 0xAA);
      size_t off = 0;
      while (true) {
        const size_t n = frameChunk(line.c_str(), len, off, maxChunk, buf.data());
        if (n == 0) break;
        ASSERT_LE(n, maxChunk) << "len " << len << " chunk " << maxChunk;
        for (size_t i = maxChunk; i < buf.size(); ++i) {
          ASSERT_EQ(buf[i], 0xAA) << "wrote past the chunk at len " << len << " chunk " << maxChunk;
        }
        joined.append(reinterpret_cast<const char*>(buf.data()), n);
        off += n;
      }
      EXPECT_EQ(joined, line + "\n") << "len " << len << " chunk " << maxChunk;
    }
  }
}

TEST(FrameChunk, RejectsEmptyChunkSizeAndOffsetsPastTheEnd) {
  uint8_t out[8];
  EXPECT_EQ(frameChunk("abc", 3, 0, 0, out), 0u);
  EXPECT_EQ(frameChunk("abc", 3, 4, 8, out), 0u);
  EXPECT_EQ(frameChunk("abc", 3, 3, 8, out), 1u);
  EXPECT_EQ(out[0], '\n');
}
