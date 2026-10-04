#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

namespace claude_buddy {

constexpr size_t kMaxLine = 4608;
constexpr size_t kReplyMax = 4096;
constexpr int kMaxEntries = 8;
constexpr uint32_t kLinkTimeoutMs = 30000;
constexpr uint32_t kTokensPerLevel = 50000;
constexpr uint32_t kCelebrateMs = 3000;
constexpr uint32_t kHeartMs = 2000;
constexpr uint32_t kHeartApproveWindowMs = 5000;

class LineFramer {
 public:
  using LineFn = void (*)(void* ctx, const char* line, size_t len);

  void feed(const uint8_t* data, size_t len, LineFn fn, void* ctx);
  void reset();

 private:
  char buf_[kMaxLine];
  size_t len_ = 0;
  bool overflow_ = false;
};

struct Prompt {
  char id[48];
  char tool[32];
  char hint[160];
};

struct State {
  uint16_t total = 0;
  uint16_t running = 0;
  uint16_t waiting = 0;
  char msg[96] = {};
  char entries[kMaxEntries][120] = {};
  uint8_t entryCount = 0;
  uint32_t tokens = 0;
  uint32_t tokensToday = 0;
  bool hasPrompt = false;
  Prompt prompt = {};
  uint32_t promptSinceMs = 0;
  char owner[33] = {};
  char name[25] = {};
  bool everReceived = false;
  uint32_t lastRxMs = 0;
  char reply[kReplyMax] = {};
  uint32_t replyVersion = 0;
};

struct Events {
  bool heartbeat = false;
  bool replyUpdated = false;
  bool newPrompt = false;
  bool promptCleared = false;
  bool completed = false;
  bool ownerChanged = false;
  bool nameChanged = false;
  bool unpair = false;
  bool statusRequested = false;
  bool timeSync = false;
  int64_t epoch = 0;
  int32_t tzOffsetSec = 0;
  char ack[96] = {};
};

struct StatusInfo {
  const char* name;
  const char* owner;
  bool secure;
  int batteryPct;
  bool usb;
  uint32_t uptimeSec;
  uint32_t freeHeap;
  uint32_t approvals;
  uint32_t denials;
  uint32_t velocitySec;
  uint32_t level;
};

class Protocol {
 public:
  Protocol();

  void handleLine(const char* line, size_t len, uint32_t nowMs, Events& events);
  const State& state() const { return state_; }
  State& state() { return state_; }

 private:
  void handleCommand(const char* cmd, JsonDocument& doc, Events& events);
  void handleHeartbeat(JsonDocument& doc, uint32_t nowMs, Events& events);
  void handleTurn(JsonDocument& doc, Events& events);

  JsonDocument filter_;
  State state_;
};

enum class Mood : uint8_t { Sleep, Idle, Busy, Attention, Celebrate, Heart };

struct MoodTimers {
  uint32_t celebrateUntilMs = 0;
  uint32_t heartUntilMs = 0;
};

bool isLinkLive(const State& state, bool transportConnected, uint32_t nowMs);
Mood deriveMood(const State& state, bool live, const MoodTimers& timers, uint32_t nowMs);

class TokenTracker {
 public:
  explicit TokenTracker(uint64_t credited = 0) : credited_(credited) {}
  bool update(uint32_t tokens);
  uint64_t credited() const { return credited_; }
  uint32_t level() const { return static_cast<uint32_t>(credited_ / kTokensPerLevel); }

 private:
  uint64_t credited_;
  uint32_t lastSeen_ = 0;
  bool hasBaseline_ = false;
};

class VelocityTracker {
 public:
  static constexpr int kSamples = 8;
  void add(uint32_t seconds);
  uint32_t median() const;
  const uint16_t* samples() const { return samples_; }
  uint8_t count() const { return count_; }
  void restore(const uint16_t* samples, uint8_t count);

 private:
  uint16_t samples_[kSamples] = {};
  uint8_t count_ = 0;
  uint8_t next_ = 0;
};

size_t copyUtf8Truncated(char* dst, size_t cap, const char* src);
size_t sanitizeName(char* dst, size_t cap, const char* src);
bool buildStatusAck(const StatusInfo& info, char* out, size_t cap);
bool buildPermission(const char* id, bool approve, char* out, size_t cap);
size_t frameChunk(const char* line, size_t len, size_t offset, size_t maxChunk, uint8_t* out);

}  // namespace claude_buddy
