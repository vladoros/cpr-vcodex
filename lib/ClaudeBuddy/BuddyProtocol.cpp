#include "BuddyProtocol.h"

#include <algorithm>
#include <cstring>

namespace claude_buddy {

namespace {

bool timeBefore(uint32_t nowMs, uint32_t untilMs) { return static_cast<int32_t>(untilMs - nowMs) > 0; }

bool writeAck(const char* cmd, bool ok, const char* error, char* out, size_t cap) {
  char name[33];
  copyUtf8Truncated(name, sizeof(name), cmd);
  JsonDocument doc;
  doc["ack"] = name;
  doc["ok"] = ok;
  doc["n"] = 0;
  if (error) doc["error"] = error;
  if (measureJson(doc) >= cap) return false;
  serializeJson(doc, out, cap);
  return true;
}

bool isTransferCommand(const char* cmd) {
  return strcmp(cmd, "char_begin") == 0 || strcmp(cmd, "file") == 0 || strcmp(cmd, "chunk") == 0 ||
         strcmp(cmd, "file_end") == 0 || strcmp(cmd, "char_end") == 0;
}

}  // namespace

void LineFramer::feed(const uint8_t* data, const size_t len, const LineFn fn, void* ctx) {
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\n' || c == '\r') {
      if (!overflow_ && len_ > 0) {
        buf_[len_] = '\0';
        fn(ctx, buf_, len_);
      }
      len_ = 0;
      overflow_ = false;
    } else if (overflow_) {
      continue;
    } else if (len_ + 1 < kMaxLine) {
      buf_[len_++] = c;
    } else {
      overflow_ = true;
      len_ = 0;
    }
  }
}

void LineFramer::reset() {
  len_ = 0;
  overflow_ = false;
}

Protocol::Protocol() {
  static const char* const kFields[] = {"total",  "running",   "waiting", "msg",  "entries", "tokens", "tokens_today",
                                        "prompt", "completed", "cmd",     "name", "time",    "evt"};
  for (const char* field : kFields) filter_[field] = true;
  filter_["role"] = true;
  filter_["content"][0]["type"] = true;
  filter_["content"][0]["text"] = true;
}

void Protocol::handleLine(const char* line, const size_t len, const uint32_t nowMs, Events& events) {
  if (len == 0 || line[0] != '{') return;
  JsonDocument doc;
  if (deserializeJson(doc, line, len, DeserializationOption::Filter(filter_))) return;

  state_.everReceived = true;
  state_.lastRxMs = nowMs;

  if (const char* evt = doc["evt"]) {
    if (strcmp(evt, "turn") == 0) handleTurn(doc, events);
    return;
  }

  if (const char* cmd = doc["cmd"]) {
    handleCommand(cmd, doc, events);
    return;
  }

  if (doc["time"].is<JsonArrayConst>()) {
    events.timeSync = true;
    events.epoch = doc["time"][0] | static_cast<int64_t>(0);
    events.tzOffsetSec = doc["time"][1] | 0;
  }

  if (!doc["total"].isNull() || !doc["running"].isNull() || !doc["waiting"].isNull() || !doc["prompt"].isNull() ||
      !doc["msg"].isNull() || !doc["entries"].isNull()) {
    handleHeartbeat(doc, nowMs, events);
  }
}

static size_t appendReplyText(char* dst, const size_t len, const size_t cap, const char* src) {
  size_t n = len;
  for (const char* p = src ? src : ""; *p && n + 1 < cap; ++p) {
    const auto c = static_cast<uint8_t>(*p);
    if (c == '\r') continue;
    dst[n++] = (c < 0x20 && c != '\n') || c == 0x7F ? ' ' : static_cast<char>(c);
  }
  while (n > len && n < cap && (static_cast<uint8_t>(dst[n - 1]) & 0x80)) {
    size_t start = n - 1;
    while (start > len && (static_cast<uint8_t>(dst[start]) & 0xC0) == 0x80) --start;
    const auto lead = static_cast<uint8_t>(dst[start]);
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - start >= need) break;
    n = start;
  }
  dst[n] = '\0';
  return n;
}

void Protocol::handleTurn(JsonDocument& doc, Events& events) {
  const char* role = doc["role"] | "";
  if (strcmp(role, "assistant") != 0) return;

  JsonArrayConst blocks = doc["content"].as<JsonArrayConst>();
  bool hasText = false;
  for (JsonVariantConst block : blocks) {
    const char* type = block["type"] | "";
    const char* text = block["text"] | "";
    if (strcmp(type, "text") == 0 && text[0] != '\0') hasText = true;
  }
  if (!hasText) return;

  char* reply = state_.reply;
  size_t len = 0;
  reply[0] = '\0';
  for (JsonVariantConst block : blocks) {
    const char* type = block["type"] | "";
    const char* text = block["text"] | "";
    if (strcmp(type, "text") != 0 || text[0] == '\0') continue;
    if (len > 0) len = appendReplyText(reply, len, kReplyMax, "\n\n");
    len = appendReplyText(reply, len, kReplyMax, text);
  }
  ++state_.replyVersion;
  events.replyUpdated = true;
}

void Protocol::handleCommand(const char* cmd, JsonDocument& doc, Events& events) {
  if (strcmp(cmd, "status") == 0) {
    events.statusRequested = true;
    return;
  }
  if (strcmp(cmd, "name") == 0 || strcmp(cmd, "owner") == 0) {
    const bool isName = cmd[0] == 'n';
    const char* value = doc["name"] | "";
    if (isName) {
      sanitizeName(state_.name, sizeof(state_.name), value);
      events.nameChanged = true;
    } else {
      sanitizeName(state_.owner, sizeof(state_.owner), value);
      events.ownerChanged = true;
    }
    writeAck(cmd, true, nullptr, events.ack, sizeof(events.ack));
    return;
  }
  if (strcmp(cmd, "unpair") == 0) {
    events.unpair = true;
    writeAck(cmd, true, nullptr, events.ack, sizeof(events.ack));
    return;
  }
  writeAck(cmd, false, isTransferCommand(cmd) ? "unsupported" : "unknown", events.ack, sizeof(events.ack));
}

void Protocol::handleHeartbeat(JsonDocument& doc, const uint32_t nowMs, Events& events) {
  events.heartbeat = true;
  state_.total = static_cast<uint16_t>(doc["total"] | 0);
  state_.running = static_cast<uint16_t>(doc["running"] | 0);
  state_.waiting = static_cast<uint16_t>(doc["waiting"] | 0);
  copyUtf8Truncated(state_.msg, sizeof(state_.msg), doc["msg"] | "");

  state_.entryCount = 0;
  for (JsonVariantConst entry : doc["entries"].as<JsonArrayConst>()) {
    if (state_.entryCount >= kMaxEntries) break;
    copyUtf8Truncated(state_.entries[state_.entryCount++], sizeof(state_.entries[0]), entry | "");
  }

  if (doc["tokens"].is<uint32_t>()) state_.tokens = doc["tokens"];
  if (doc["tokens_today"].is<uint32_t>()) state_.tokensToday = doc["tokens_today"];
  events.completed = doc["completed"] | false;

  const char* promptId = doc["prompt"]["id"];
  if (promptId && promptId[0] != '\0') {
    Prompt next = {};
    copyUtf8Truncated(next.id, sizeof(next.id), promptId);
    copyUtf8Truncated(next.tool, sizeof(next.tool), doc["prompt"]["tool"] | "");
    copyUtf8Truncated(next.hint, sizeof(next.hint), doc["prompt"]["hint"] | "");
    if (!state_.hasPrompt || strcmp(state_.prompt.id, next.id) != 0) {
      events.newPrompt = true;
      state_.promptSinceMs = nowMs;
    }
    state_.prompt = next;
    state_.hasPrompt = true;
  } else if (state_.hasPrompt) {
    state_.hasPrompt = false;
    state_.prompt = {};
    events.promptCleared = true;
  }
}

bool isLinkLive(const State& state, const bool transportConnected, const uint32_t nowMs) {
  return transportConnected && state.everReceived && (nowMs - state.lastRxMs) < kLinkTimeoutMs;
}

Mood deriveMood(const State& state, const bool live, const MoodTimers& timers, const uint32_t nowMs) {
  if (!live) return Mood::Sleep;
  if (state.hasPrompt || state.waiting > 0) return Mood::Attention;
  if (timeBefore(nowMs, timers.heartUntilMs)) return Mood::Heart;
  if (timeBefore(nowMs, timers.celebrateUntilMs)) return Mood::Celebrate;
  if (state.running > 0) return Mood::Busy;
  return Mood::Idle;
}

bool TokenTracker::update(const uint32_t tokens) {
  if (!hasBaseline_ || tokens < lastSeen_) {
    hasBaseline_ = true;
    lastSeen_ = tokens;
    return false;
  }
  const uint32_t before = level();
  credited_ += tokens - lastSeen_;
  lastSeen_ = tokens;
  return level() > before;
}

void VelocityTracker::add(const uint32_t seconds) {
  samples_[next_] = static_cast<uint16_t>(seconds > 0xFFFF ? 0xFFFF : seconds);
  next_ = static_cast<uint8_t>((next_ + 1) % kSamples);
  if (count_ < kSamples) ++count_;
}

uint32_t VelocityTracker::median() const {
  if (count_ == 0) return 0;
  uint16_t sorted[kSamples];
  memcpy(sorted, samples_, sizeof(sorted));
  for (int i = 1; i < count_; ++i) {
    const uint16_t v = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > v) {
      sorted[j + 1] = sorted[j];
      --j;
    }
    sorted[j + 1] = v;
  }
  return sorted[count_ / 2];
}

void VelocityTracker::restore(const uint16_t* samples, const uint8_t count) {
  count_ = count > kSamples ? kSamples : count;
  memcpy(samples_, samples, sizeof(uint16_t) * count_);
  next_ = static_cast<uint8_t>(count_ % kSamples);
}

size_t copyUtf8Truncated(char* dst, const size_t cap, const char* src) {
  if (cap == 0) return 0;
  if (!src) src = "";
  size_t n = strnlen(src, cap);
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) --n;
  }
  for (size_t i = 0; i < n; ++i) {
    const auto c = static_cast<uint8_t>(src[i]);
    dst[i] = c < 0x20 ? ' ' : static_cast<char>(c);
  }
  dst[n] = '\0';
  return n;
}

size_t sanitizeName(char* dst, const size_t cap, const char* src) {
  if (cap == 0) return 0;
  size_t n = 0;
  for (const char* p = src ? src : ""; *p && n + 1 < cap; ++p) {
    const auto c = static_cast<uint8_t>(*p);
    if (c < 0x20 || c == 0x7F || c == '"' || c == '\\') continue;
    dst[n++] = static_cast<char>(c);
  }
  while (n > 0 && (static_cast<uint8_t>(dst[n - 1]) & 0x80)) {
    size_t start = n - 1;
    while (start > 0 && (static_cast<uint8_t>(dst[start]) & 0xC0) == 0x80) --start;
    const auto lead = static_cast<uint8_t>(dst[start]);
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - start >= need) break;
    n = start;
  }
  dst[n] = '\0';
  return n;
}

bool buildStatusAck(const StatusInfo& info, char* out, const size_t cap) {
  JsonDocument doc;
  doc["ack"] = "status";
  doc["ok"] = true;
  doc["n"] = 0;
  JsonObject data = doc["data"].to<JsonObject>();
  data["name"] = info.name ? info.name : "";
  data["owner"] = info.owner ? info.owner : "";
  data["sec"] = info.secure;
  if (info.batteryPct >= 0) {
    JsonObject bat = data["bat"].to<JsonObject>();
    bat["pct"] = info.batteryPct;
    bat["usb"] = info.usb;
  }
  JsonObject sys = data["sys"].to<JsonObject>();
  sys["up"] = info.uptimeSec;
  sys["heap"] = info.freeHeap;
  JsonObject stats = data["stats"].to<JsonObject>();
  stats["appr"] = info.approvals;
  stats["deny"] = info.denials;
  stats["vel"] = info.velocitySec;
  stats["lvl"] = info.level;
  if (measureJson(doc) >= cap) return false;
  serializeJson(doc, out, cap);
  return true;
}

bool buildPermission(const char* id, const bool approve, char* out, const size_t cap) {
  JsonDocument doc;
  doc["cmd"] = "permission";
  doc["id"] = id;
  doc["decision"] = approve ? "once" : "deny";
  if (measureJson(doc) >= cap) return false;
  serializeJson(doc, out, cap);
  return true;
}

size_t frameChunk(const char* line, const size_t len, const size_t offset, const size_t maxChunk, uint8_t* out) {
  const size_t total = len + 1;
  if (maxChunk == 0 || offset >= total) return 0;
  const size_t n = std::min(maxChunk, total - offset);
  const size_t copy = offset < len ? std::min(n, len - offset) : 0;
  memcpy(out, line + offset, copy);
  if (copy < n) out[copy] = '\n';
  return n;
}

}  // namespace claude_buddy
