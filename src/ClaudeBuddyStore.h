#pragma once
#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_CLAUDE_BUDDY

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>

class ClaudeBuddyStore : public PersistableStore<ClaudeBuddyStore> {
  ClaudeBuddyStore() = default;
  friend class PersistableStore<ClaudeBuddyStore>;

 public:
  static constexpr int kVelocitySamples = 8;

  char name[25] = {};
  char owner[33] = {};
  uint32_t approvals = 0;
  uint32_t denials = 0;
  uint64_t tokensCredited = 0;
  uint16_t velocity[kVelocitySamples] = {};
  uint8_t velocityCount = 0;
  uint8_t species = 0;

  static const char* getFilePath() { return "/.crosspoint/claude_buddy.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
};

#define CLAUDE_BUDDY_STORE ClaudeBuddyStore::getInstance()

#endif
