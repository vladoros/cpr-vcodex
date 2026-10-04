#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_CLAUDE_BUDDY && defined(SIMULATOR)

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "BuddyTransport.h"

namespace {
struct Step {
  uint32_t atMs;
  const char* line;
};

constexpr Step kScript[] = {
    {0, R"({"time":[1775731234,-25200]})"},
    {0, R"({"cmd":"owner","name":"Felix"})"},
    {0, R"({"cmd":"name","name":"Clawd"})"},
    {0, R"({"total":0,"running":0,"waiting":0,"msg":"idle","entries":[],"tokens":0,"tokens_today":1200})"},
    {3000,
     R"({"evt":"turn","role":"assistant","content":[{"type":"text","text":"I found the failing test: the cache key ignored the locale, so two languages shared one entry.\n\nThe fix is a one-line change to the key builder. I will update it and re-run the suite.\n\nWhat I changed:\n- The key builder now includes the locale\n- Existing entries are migrated on first read\n- A regression test covers two locales\n\nWhy it matters: before this change, switching the language could show a cached page in the previous language until the cache expired. The new key avoids that without clearing anything.\n\nRisks I checked: the migration only reads old entries and never deletes them, so a failed run leaves the cache exactly as it was. The key change is backward compatible for single-language installs.\n\nOne open question: should the locale also be part of the cache directory name? That would make manual cleanup easier, but it changes the on-disk layout, so I left it out of this change.\n\nNext steps:\n- Re-run the full suite\n- Check the migration on a copy of the real cache\n- Open a pull request with the summary above"}]})"},
    {6000,
     R"({"total":1,"running":1,"waiting":0,"msg":"working","entries":["10:41 yarn test","10:40 editing cache.ts","10:39 reading file...","10:38 grep locale","10:37 reading file...","10:36 git status","10:35 reading file...","10:34 started"],"tokens":900,"tokens_today":2100})"},
    {12000,
     R"({"total":1,"running":1,"waiting":1,"msg":"approve: Bash","entries":["10:42 git push","10:41 yarn test"],"tokens":1800,"tokens_today":3000,"prompt":{"id":"req_demo","tool":"Bash","hint":"rm -rf /tmp/foo"}})"},
};
constexpr size_t kScriptLen = sizeof(kScript) / sizeof(kScript[0]);
constexpr uint32_t kConnectAtMs = 1500;
constexpr uint32_t kSecureAtMs = 4000;
constexpr size_t kIdleStep = 3;
constexpr char kCompletedLine[] =
    R"({"total":1,"running":0,"waiting":0,"msg":"done","entries":["10:43 done"],"tokens":2600,"tokens_today":3800,"completed":true})";
constexpr uint32_t kResolvedHoldMs = 6000;
}  // namespace

class DemoBuddyTransport final : public BuddyTransport {
 public:
  bool begin() override {
    startMs_ = millis();
    next_ = 0;
    resolvedAtMs_ = 0;
    return true;
  }
  void end() override { pending_.clear(); }

  size_t read(uint8_t* out, size_t cap) override {
    if (secure()) {
      const uint32_t now = millis();
      const uint32_t rel = now - startMs_ - kSecureAtMs;
      while (next_ < kScriptLen && kScript[next_].atMs <= rel) enqueue(kScript[next_++].line);
      if (resolvedAtMs_ != 0 && now - resolvedAtMs_ >= 300) {
        enqueue(kCompletedLine);
        resolvedAtMs_ = 0;
        restartAtMs_ = now + kResolvedHoldMs;
      }
      if (restartAtMs_ != 0 && now >= restartAtMs_) {
        restartAtMs_ = 0;
        startMs_ = now - kSecureAtMs;
        next_ = kIdleStep;
      }
    }
    const size_t n = std::min(cap, pending_.size());
    memcpy(out, pending_.data(), n);
    pending_.erase(0, n);
    return n;
  }

  bool send(const char* line) override {
    LOG_INF("BUDDY", "demo tx: %s", line);
    if (strstr(line, "\"permission\"")) resolvedAtMs_ = millis();
    return true;
  }
  bool connected() const override { return millis() - startMs_ >= kConnectAtMs; }
  bool secure() const override { return millis() - startMs_ >= kSecureAtMs; }
  uint32_t passkey() const override { return connected() && !secure() ? 123456 : 0; }
  void deleteBonds() override { LOG_INF("BUDDY", "demo: bonds deleted"); }
  const char* advertisedName() const override { return "Claude-DEMO"; }

 private:
  void enqueue(const char* line) {
    pending_ += line;
    pending_ += '\n';
  }

  std::string pending_;
  uint32_t startMs_ = 0;
  uint32_t resolvedAtMs_ = 0;
  uint32_t restartAtMs_ = 0;
  size_t next_ = 0;
};

std::unique_ptr<BuddyTransport> makeDemoBuddyTransport() { return makeUniqueNoThrow<DemoBuddyTransport>(); }

#endif
