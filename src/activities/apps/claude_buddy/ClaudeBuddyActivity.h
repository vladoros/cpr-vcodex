#pragma once

#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_CLAUDE_BUDDY

#include <BuddyProtocol.h>

#include <memory>
#include <string>
#include <vector>

#include "BuddyTransport.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class ClaudeBuddyActivity final : public Activity {
 public:
  ClaudeBuddyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class Screen : uint8_t { Error, Pairing, Waiting, Dashboard, Approval };
  enum class Page : uint8_t { Status, Reply, Stats, Options };
  static constexpr int kPageCount = 4;
  static constexpr int kOptionCount = 2;
  struct LineContext {
    ClaudeBuddyActivity* self;
    uint32_t nowMs;
  };

  static void onLine(void* ctx, const char* line, size_t len);

  Screen currentScreen() const;
  bool isPaged(Screen screen) const { return screen == Screen::Dashboard || screen == Screen::Waiting; }
  bool pageScrolls() const;
  void drainTransport(uint32_t nowMs);
  void applyEvents(const claude_buddy::Events& events, uint32_t nowMs);
  void sendStatus();
  void decide(bool approve, uint32_t nowMs);
  void toggleForget(uint32_t nowMs);
  bool changePage(int delta);
  void scrollBy(int delta);
  void activateOption(uint32_t nowMs);
  void saveSpecies();
  void handleInput(Screen screen, uint32_t nowMs);
  void exitApp();
  uint32_t contentSignature(Screen screen, claude_buddy::Mood mood, uint32_t nowMs) const;
  void saveStore();

  int drawPet(claude_buddy::Mood mood, int y) const;
  void drawScrollIndicator(const Rect& area, int y, int first, int last, int total) const;
  void renderPairing(const Rect& area) const;
  void renderWaiting(const Rect& area) const;
  void renderStatus(const Rect& area, claude_buddy::Mood mood, int bottom);
  void renderReply(const Rect& area, int bottom);
  void renderStats(const Rect& area, int bottom) const;
  void renderOptions(const Rect& area, claude_buddy::Mood mood) const;
  void renderApproval(const Rect& area, claude_buddy::Mood mood, int bottom) const;

  std::unique_ptr<BuddyTransport> transport_;
  std::unique_ptr<claude_buddy::Protocol> protocol_;
  std::unique_ptr<claude_buddy::LineFramer> framer_;
  std::unique_ptr<char[]> txBuf_;
  claude_buddy::MoodTimers timers_;
  claude_buddy::TokenTracker tokens_;
  claude_buddy::VelocityTracker velocity_;
  ScreenTransitionRefresh screenTransitionRefresh_;
  ButtonNavigator buttonNavigator_;

  // Wrapped reply lines are rebuilt only when the reply text or the text width changes.
  std::vector<std::string> replyLines_;
  uint32_t replyCacheVersion_ = 0;
  int replyCacheWidth_ = 0;

  bool failed_ = false;
  bool decisionSent_ = false;
  bool storeDirty_ = false;
  bool inputDirty_ = false;
  bool forgetArmed_ = false;
  Page page_ = Page::Status;
  uint8_t species_ = 0;
  int optionIndex_ = 0;
  int entryScroll_ = 0;
  int replyScroll_ = 0;
  int entryRows_ = 0;
  int replyRows_ = 0;
  int replyLineCount_ = 0;
  uint32_t approvals_ = 0;
  uint32_t denials_ = 0;
  uint32_t forgetArmedAtMs_ = 0;
  uint32_t forgottenAtMs_ = 0;
  uint32_t lastSig_ = 0;
  uint32_t lastRenderMs_ = 0;
  uint32_t lastSaveMs_ = 0;
  Screen lastScreen_ = Screen::Waiting;
  uint8_t fastRefreshCount_ = 0;
};

#endif
