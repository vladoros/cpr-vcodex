#pragma once

#include <cstdint>

// Shared Wi-Fi, clock and memory helpers for the network apps (Weather, Stock
// Ticker). They run in-process, so each keeps its own connection lifetime and
// tears Wi-Fi down on exit when it brought the radio up.
namespace AppNetwork {

constexpr uint32_t kSilentConnectTimeoutMs = 10000;
// mbedTLS needs roughly 35-40 KB of internal heap for one HTTPS handshake;
// below this the request is skipped and cached data is shown instead.
constexpr uint32_t kMinTlsFreeHeap = 40000;
constexpr uint32_t kMinTlsMaxAlloc = 16384;

bool isConnected();

// True when a saved network exists that beginSilentConnect() can try.
bool hasSavedNetwork();

// Starts a non-blocking connect to the last saved network; poll isConnected().
// Returns false when there is no saved credential to try.
bool beginSilentConnect();

// Disconnects and powers the radio down.
void tearDown();

// time(nullptr) holds a plausible wall clock (set by the RTC sync or NTP).
bool clockValid();

// Syncs the system clock over NTP when it is not valid yet. Blocks up to ~5 s;
// call while showing a status screen. Returns clockValid() afterwards.
bool ensureClock();

// Logs internal heap and reports whether a TLS handshake is likely to fit.
bool hasTlsHeadroom(const char* tag);

}  // namespace AppNetwork
