#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class BuddyTransport {
 public:
  virtual ~BuddyTransport() = default;
  virtual bool begin() = 0;
  virtual void end() = 0;
  virtual size_t read(uint8_t* out, size_t cap) = 0;
  virtual bool send(const char* line) = 0;
  virtual bool connected() const = 0;
  virtual bool secure() const = 0;
  virtual uint32_t passkey() const = 0;
  virtual void deleteBonds() = 0;
  virtual const char* advertisedName() const = 0;
};

std::unique_ptr<BuddyTransport> makeBuddyTransport();
std::unique_ptr<BuddyTransport> makeDemoBuddyTransport();
std::unique_ptr<BuddyTransport> makeBleBuddyTransport();
