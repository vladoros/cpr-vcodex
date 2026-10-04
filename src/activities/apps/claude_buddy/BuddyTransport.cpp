#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_CLAUDE_BUDDY

#include "BuddyTransport.h"

std::unique_ptr<BuddyTransport> makeBuddyTransport() {
#ifdef SIMULATOR
  return makeDemoBuddyTransport();
#else
  return makeBleBuddyTransport();
#endif
}

#endif
