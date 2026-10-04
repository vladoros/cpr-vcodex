#include "ClaudeBuddyStore.h"

#if CROSSINK_APP_CAP_CLAUDE_BUDDY

#include <cstring>

void ClaudeBuddyStore::toJson(JsonDocument& doc) const {
  doc["name"] = name;
  doc["owner"] = owner;
  doc["approvals"] = approvals;
  doc["denials"] = denials;
  doc["tokens"] = tokensCredited;
  doc["species"] = species;
  JsonArray samples = doc["velocity"].to<JsonArray>();
  for (int i = 0; i < velocityCount; ++i) samples.add(velocity[i]);
}

bool ClaudeBuddyStore::fromJson(JsonVariantConst doc) {
  strlcpy(name, doc["name"] | "", sizeof(name));
  strlcpy(owner, doc["owner"] | "", sizeof(owner));
  approvals = doc["approvals"] | 0u;
  denials = doc["denials"] | 0u;
  tokensCredited = doc["tokens"] | static_cast<uint64_t>(0);
  species = doc["species"] | 0;
  velocityCount = 0;
  for (JsonVariantConst v : doc["velocity"].as<JsonArrayConst>()) {
    if (velocityCount >= kVelocitySamples) break;
    velocity[velocityCount++] = v | 0;
  }
  return true;
}

#endif
