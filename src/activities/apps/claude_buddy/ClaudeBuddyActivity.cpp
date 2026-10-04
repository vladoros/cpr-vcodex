#include "ClaudeBuddyActivity.h"

#if CROSSINK_APP_CAP_CLAUDE_BUDDY

#include <Arduino.h>
#include <BuddyPet.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontSystem.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ClaudeBuddyStore.h"
#include "MappedInputManager.h"
#include "components/CompactHeader.h"
#include "components/PageDots.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

#ifndef SIMULATOR
#include <esp_heap_caps.h>
#endif

using namespace claude_buddy;

namespace {
constexpr uint32_t kMinRenderGapMs = 1500;
constexpr uint32_t kSaveIntervalMs = 60000;
constexpr uint32_t kForgetConfirmMs = 5000;
constexpr uint32_t kForgottenShownMs = 3000;
constexpr uint8_t kFullRefreshEvery = 12;
constexpr size_t kTxBufBytes = 320;
constexpr size_t kReadChunk = 256;
constexpr int kMaxReadChunksPerLoop = 8;
constexpr int kMaxReplyLines = 400;
constexpr int kMaxParagraphLines = 200;

constexpr StrId kPetNames[kPetSpeciesCount] = {StrId::STR_CLAUDE_BUDDY_PET_CAT,   StrId::STR_CLAUDE_BUDDY_PET_CAPYBARA,
                                               StrId::STR_CLAUDE_BUDDY_PET_GHOST, StrId::STR_CLAUDE_BUDDY_PET_ROBOT,
                                               StrId::STR_CLAUDE_BUDDY_PET_OWL,   StrId::STR_CLAUDE_BUDDY_PET_RABBIT};

uint32_t freeHeap() {
#ifdef SIMULATOR
  return 0;
#else
  return ESP.getFreeHeap();
#endif
}

uint32_t freeInternalHeap() {
#ifdef SIMULATOR
  return 0;
#else
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#endif
}

uint32_t hashBytes(uint32_t h, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

uint32_t hashStr(uint32_t h, const char* s) { return hashBytes(h, s, strlen(s)); }

template <typename T>
uint32_t hashValue(uint32_t h, const T& v) {
  return hashBytes(h, &v, sizeof(v));
}

int pagedBottom(const GfxRenderer& renderer) {
  return pageDotsY(renderer) - UITheme::getInstance().getMetrics().verticalSpacing;
}

int approvalBottom(const GfxRenderer& renderer) {
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return screen.y + screen.height - UITheme::getInstance().getMetrics().verticalSpacing;
}

TouchActionButtons::Layout actionLayout(const GfxRenderer& renderer, const uint8_t count, const int bottom) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int totalHeight =
      TouchActionButtons::kDefaultHeight * count + TouchActionButtons::kDefaultGap * (count > 0 ? count - 1 : 0);
  const Rect container{screen.x + metrics.contentSidePadding, bottom - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, count);
}

const char* moodLabel(const Mood mood) {
  switch (mood) {
    case Mood::Sleep:
      return tr(STR_CLAUDE_BUDDY_MOOD_SLEEP);
    case Mood::Idle:
      return tr(STR_CLAUDE_BUDDY_MOOD_IDLE);
    case Mood::Busy:
      return tr(STR_CLAUDE_BUDDY_MOOD_BUSY);
    case Mood::Attention:
      return tr(STR_CLAUDE_BUDDY_MOOD_ATTENTION);
    case Mood::Celebrate:
      return tr(STR_CLAUDE_BUDDY_MOOD_CELEBRATE);
    case Mood::Heart:
      return tr(STR_CLAUDE_BUDDY_MOOD_HEART);
  }
  return "";
}

const char* pageTitle(const int page) {
  switch (page) {
    case 0:
      return tr(STR_CLAUDE_BUDDY);
    case 1:
      return tr(STR_CLAUDE_BUDDY_PAGE_REPLY);
    case 2:
      return tr(STR_CLAUDE_BUDDY_PAGE_STATS);
    default:
      return tr(STR_CLAUDE_BUDDY_PAGE_OPTIONS);
  }
}

void buildWho(const State& s, char* out, const size_t cap) {
  out[0] = '\0';
  if (s.owner[0] != '\0' && s.name[0] != '\0') {
    snprintf(out, cap, tr(STR_CLAUDE_BUDDY_OWNER_OF), s.owner, s.name);
  } else if (s.name[0] != '\0') {
    strlcpy(out, s.name, cap);
  } else if (s.owner[0] != '\0') {
    snprintf(out, cap, tr(STR_CLAUDE_BUDDY_OWNER_OF), s.owner, tr(STR_CLAUDE_BUDDY));
  }
}
}  // namespace

ClaudeBuddyActivity::ClaudeBuddyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ClaudeBuddy", renderer, mappedInput) {}

void ClaudeBuddyActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);
  LOG_INF("BUDDY", "Enter: free heap %u, internal %u", freeHeap(), freeInternalHeap());

  transport_ = makeBuddyTransport();
  protocol_ = makeUniqueNoThrow<Protocol>();
  framer_ = makeUniqueNoThrow<LineFramer>();
  txBuf_ = makeUniqueNoThrow<char[]>(kTxBufBytes);
  if (!transport_ || !protocol_ || !framer_ || !txBuf_) {
    LOG_ERR("BUDDY", "Cannot allocate Claude Buddy state");
    failed_ = true;
    requestUpdate();
    return;
  }

  auto& store = CLAUDE_BUDDY_STORE;
  store.ensureLoaded();
  species_ = store.species < kPetSpeciesCount ? store.species : 0;
  strlcpy(protocol_->state().name, store.name, sizeof(protocol_->state().name));
  strlcpy(protocol_->state().owner, store.owner, sizeof(protocol_->state().owner));
  approvals_ = store.approvals;
  denials_ = store.denials;
  tokens_ = TokenTracker(store.tokensCredited);
  velocity_.restore(store.velocity, store.velocityCount);

  failed_ = !transport_->begin();
  LOG_INF("BUDDY", "BLE %s: free heap %u, internal %u", failed_ ? "failed" : "started", freeHeap(), freeInternalHeap());
  lastSaveMs_ = millis();
  requestUpdate();
}

void ClaudeBuddyActivity::onExit() {
  if (transport_) transport_->end();
  saveStore();
  transport_.reset();
  framer_.reset();
  protocol_.reset();
  txBuf_.reset();
  LOG_INF("BUDDY", "Exit: free heap %u, internal %u", freeHeap(), freeInternalHeap());
  Activity::onExit();
}

void ClaudeBuddyActivity::exitApp() { finishAfterBackPress(); }

void ClaudeBuddyActivity::saveStore() {
  if (!storeDirty_ || !protocol_) return;
  auto& store = CLAUDE_BUDDY_STORE;
  strlcpy(store.name, protocol_->state().name, sizeof(store.name));
  strlcpy(store.owner, protocol_->state().owner, sizeof(store.owner));
  store.approvals = approvals_;
  store.denials = denials_;
  store.tokensCredited = tokens_.credited();
  store.velocityCount = velocity_.count();
  memcpy(store.velocity, velocity_.samples(), sizeof(uint16_t) * velocity_.count());
  store.species = species_;
  store.saveToFile();
  storeDirty_ = false;
}

void ClaudeBuddyActivity::saveSpecies() {
  auto& store = CLAUDE_BUDDY_STORE;
  store.species = species_;
  store.saveToFile();
}

ClaudeBuddyActivity::Screen ClaudeBuddyActivity::currentScreen() const {
  if (failed_ || !transport_) return Screen::Error;
  if (!transport_->connected()) return Screen::Waiting;
  if (!transport_->secure() && transport_->passkey() != 0) return Screen::Pairing;
  if (protocol_->state().hasPrompt) return Screen::Approval;
  return Screen::Dashboard;
}

void ClaudeBuddyActivity::onLine(void* ctx, const char* line, const size_t len) {
  auto* lc = static_cast<LineContext*>(ctx);
  Events events;
  lc->self->protocol_->handleLine(line, len, lc->nowMs, events);
  lc->self->applyEvents(events, lc->nowMs);
}

void ClaudeBuddyActivity::drainTransport(const uint32_t nowMs) {
  uint8_t chunk[kReadChunk];
  LineContext ctx{this, nowMs};
  for (int i = 0; i < kMaxReadChunksPerLoop; ++i) {
    const size_t n = transport_->read(chunk, sizeof(chunk));
    if (n == 0) break;
    RenderLock lock(*this);
    framer_->feed(chunk, n, &ClaudeBuddyActivity::onLine, &ctx);
  }
}

void ClaudeBuddyActivity::applyEvents(const Events& events, const uint32_t nowMs) {
  const State& state = protocol_->state();

  if (events.ack[0] != '\0') transport_->send(events.ack);
  if (events.statusRequested) sendStatus();
  if (events.unpair) {
    LOG_INF("BUDDY", "Desktop sent unpair");
    transport_->deleteBonds();
  }
  if (events.nameChanged || events.ownerChanged) storeDirty_ = true;
  if (events.newPrompt || events.promptCleared) decisionSent_ = false;
  if (events.completed) timers_.celebrateUntilMs = nowMs + kCelebrateMs;

  if (events.heartbeat && tokens_.update(state.tokens)) {
    timers_.celebrateUntilMs = nowMs + kCelebrateMs;
    storeDirty_ = true;
  }
}

void ClaudeBuddyActivity::sendStatus() {
  const State& state = protocol_->state();
  const StatusInfo info{state.name,
                        state.owner,
                        transport_->secure(),
                        static_cast<int>(powerManager.getBatteryPercentage()),
                        gpio.isUsbConnected(),
                        millis() / 1000,
                        freeHeap(),
                        approvals_,
                        denials_,
                        velocity_.median(),
                        tokens_.level()};
  if (buildStatusAck(info, txBuf_.get(), kTxBufBytes)) {
    transport_->send(txBuf_.get());
  } else {
    LOG_ERR("BUDDY", "Status ack does not fit");
  }
}

void ClaudeBuddyActivity::decide(const bool approve, const uint32_t nowMs) {
  const State& state = protocol_->state();
  if (!state.hasPrompt || decisionSent_) return;
  if (!buildPermission(state.prompt.id, approve, txBuf_.get(), kTxBufBytes) || !transport_->send(txBuf_.get())) {
    LOG_ERR("BUDDY", "Could not send permission decision");
    return;
  }
  decisionSent_ = true;
  if (approve) {
    ++approvals_;
    const uint32_t waitedMs = nowMs - state.promptSinceMs;
    velocity_.add(waitedMs / 1000);
    if (waitedMs < kHeartApproveWindowMs) timers_.heartUntilMs = nowMs + kHeartMs;
  } else {
    ++denials_;
  }
  storeDirty_ = true;
}

void ClaudeBuddyActivity::toggleForget(const uint32_t nowMs) {
  if (forgetArmed_) {
    transport_->deleteBonds();
    forgetArmed_ = false;
    forgottenAtMs_ = nowMs;
  } else {
    forgetArmed_ = true;
    forgetArmedAtMs_ = nowMs;
  }
  inputDirty_ = true;
}

bool ClaudeBuddyActivity::changePage(const int delta) {
  const int target = static_cast<int>(page_) + delta;
  if (target < 0 || target >= kPageCount) return false;
  RenderLock lock(*this);
  page_ = static_cast<Page>(target);
  inputDirty_ = true;
  return true;
}

bool ClaudeBuddyActivity::pageScrolls() const {
  switch (page_) {
    case Page::Status:
      return protocol_->state().entryCount > entryRows_;
    case Page::Reply:
      return replyLineCount_ > replyRows_;
    case Page::Options:
      return true;
    case Page::Stats:
      return false;
  }
  return false;
}

void ClaudeBuddyActivity::scrollBy(const int delta) {
  RenderLock lock(*this);
  switch (page_) {
    case Page::Status:
      entryScroll_ = std::max(0, entryScroll_ + delta);
      break;
    case Page::Reply:
      replyScroll_ = std::max(0, replyScroll_ + delta);
      break;
    case Page::Options:
      optionIndex_ = (optionIndex_ + (delta > 0 ? 1 : -1) + kOptionCount) % kOptionCount;
      break;
    case Page::Stats:
      return;
  }
  inputDirty_ = true;
}

void ClaudeBuddyActivity::activateOption(const uint32_t nowMs) {
  if (optionIndex_ == 0) {
    species_ = static_cast<uint8_t>((species_ + 1) % kPetSpeciesCount);
    saveSpecies();
    inputDirty_ = true;
  } else {
    toggleForget(nowMs);
  }
}

void ClaudeBuddyActivity::handleInput(const Screen screen, const uint32_t nowMs) {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    exitApp();
    return;
  }

  const bool approvalOpen = screen == Screen::Approval && !decisionSent_;
  const bool paged = isPaged(screen);
  const bool optionsPage = paged && page_ == Page::Options;
  const auto swipe = mappedInput.wasSwipe();

  if (mappedInput.hasTouch() && (approvalOpen || optionsPage)) {
    const uint8_t count = approvalOpen ? 2 : kOptionCount;
    const auto actions = actionLayout(renderer, count, approvalOpen ? approvalBottom(renderer) : pagedBottom(renderer));
    int touchedAction = -1;
    const auto touch = mappedInput.rowTouch(touchedAction, actions.buttons[0].y,
                                            TouchActionButtons::kDefaultHeight + TouchActionButtons::kDefaultGap,
                                            actions.count, actions.buttons[0].x,
                                            actions.buttons[0].x + actions.buttons[0].width, actions.buttons[0].height);
    if (touch == MappedInputManager::RowTouch::Down) return;
    if (touch == MappedInputManager::RowTouch::Tap) {
      if (approvalOpen) {
        decide(touchedAction == 0, nowMs);
      } else {
        optionIndex_ = touchedAction;
        activateOption(nowMs);
      }
      return;
    }
  }

  if (paged) {
    if (swipe == MappedInputManager::SwipeDir::Left || mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      changePage(1);
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Right) {
      if (!changePage(-1)) exitApp();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      changePage(-1);
      return;
    }
    if (pageScrolls()) {
      const int rows = page_ == Page::Reply ? replyRows_ : entryRows_;
      const int pageStep = std::max(1, rows - 1);
      if (swipe == MappedInputManager::SwipeDir::Up) scrollBy(pageStep);
      if (swipe == MappedInputManager::SwipeDir::Down) scrollBy(-pageStep);
      buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { scrollBy(-1); });
      buttonNavigator_.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { scrollBy(1); });
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      changePage(-1);
      return;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      changePage(1);
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    if (approvalOpen) {
      decide(true, nowMs);
    } else if (optionsPage) {
      activateOption(nowMs);
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (approvalOpen) {
      decide(false, nowMs);
    } else {
      exitApp();
    }
  }
}

uint32_t ClaudeBuddyActivity::contentSignature(const Screen screen, const Mood mood, const uint32_t nowMs) const {
  uint32_t h = 2166136261u;
  h = hashValue(h, screen);
  h = hashValue(h, mood);
  h = hashValue(h, page_);
  h = hashValue(h, species_);
  h = hashValue(h, decisionSent_);
  h = hashValue(h, forgetArmed_);
  h = hashValue(h, forgottenAtMs_ != 0 && nowMs - forgottenAtMs_ < kForgottenShownMs);
  if (!transport_) return h;
  h = hashValue(h, transport_->passkey());
  const State& s = protocol_->state();
  h = hashValue(h, s.total);
  h = hashValue(h, s.running);
  h = hashValue(h, s.waiting);
  h = hashStr(h, s.msg);
  h = hashStr(h, s.owner);
  h = hashStr(h, s.name);
  for (int i = 0; i < s.entryCount; ++i) h = hashStr(h, s.entries[i]);
  if (s.hasPrompt) h = hashStr(hashStr(h, s.prompt.id), s.prompt.hint);
  switch (page_) {
    case Page::Status:
      h = hashValue(h, entryScroll_);
      break;
    case Page::Reply:
      h = hashValue(h, replyScroll_);
      h = hashValue(h, s.replyVersion);
      break;
    case Page::Stats:
      h = hashValue(h, approvals_);
      h = hashValue(h, denials_);
      h = hashValue(h, s.tokensToday);
      h = hashValue(h, s.tokens);
      h = hashValue(h, tokens_.level());
      h = hashValue(h, transport_->secure());
      break;
    case Page::Options:
      h = hashValue(h, optionIndex_);
      break;
  }
  return h;
}

void ClaudeBuddyActivity::loop() {
  const uint32_t nowMs = millis();
  const Screen screen = currentScreen();

  if (screen == Screen::Error) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      exitApp();
    }
    return;
  }

  drainTransport(nowMs);
  handleInput(screen, nowMs);

  if (forgetArmed_ && nowMs - forgetArmedAtMs_ >= kForgetConfirmMs) forgetArmed_ = false;
  if (storeDirty_ && nowMs - lastSaveMs_ >= kSaveIntervalMs) {
    saveStore();
    lastSaveMs_ = nowMs;
  }

  entryScroll_ = std::clamp(entryScroll_, 0, std::max(0, static_cast<int>(protocol_->state().entryCount) - entryRows_));
  replyScroll_ = std::clamp(replyScroll_, 0, std::max(0, replyLineCount_ - replyRows_));

  const Screen shown = currentScreen();
  const bool live = isLinkLive(protocol_->state(), transport_->connected(), nowMs);
  const Mood mood = deriveMood(protocol_->state(), live, timers_, nowMs);
  const uint32_t sig = contentSignature(shown, mood, nowMs);
  if (sig != lastSig_ && (inputDirty_ || shown != lastScreen_ || nowMs - lastRenderMs_ >= kMinRenderGapMs)) {
    lastSig_ = sig;
    lastScreen_ = shown;
    lastRenderMs_ = nowMs;
    inputDirty_ = false;
    requestUpdate();
  }
}

int ClaudeBuddyActivity::drawPet(const Mood mood, const int y) const {
  constexpr int fontId = UI_10_FONT_ID;
  const int cellW = renderer.getTextWidth(fontId, "n");
  const int lineH = renderer.getLineHeight(fontId);
  const int x0 = (renderer.getScreenWidth() - cellW * kPetCols) / 2;
  const char* const* frame = petFrame(species_, mood);
  for (int r = 0; r < kPetRows; ++r) {
    for (int c = 0; c < kPetCols; ++c) {
      if (frame[r][c] == ' ') continue;
      const char glyph[2] = {frame[r][c], '\0'};
      const int w = renderer.getTextWidth(fontId, glyph);
      renderer.drawText(fontId, x0 + c * cellW + (cellW - w) / 2, y + r * lineH, glyph);
    }
  }
  return kPetRows * lineH;
}

void ClaudeBuddyActivity::drawScrollIndicator(const Rect& area, const int y, const int first, const int last,
                                              const int total) const {
  char text[32];
  snprintf(text, sizeof(text), "%d-%d/%d", first, last, total);
  const int w = renderer.getTextWidth(SMALL_FONT_ID, text);
  renderer.drawText(SMALL_FONT_ID, area.x + area.width - w, y, text);
}

void ClaudeBuddyActivity::renderPairing(const Rect& area) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int y = area.y + area.height / 4;
  y += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, tr(STR_CLAUDE_BUDDY_PAIRING), 2, true,
                                        EpdFontFamily::BOLD);
  y += metrics.verticalSpacing * 2;
  char code[16];
  const uint32_t key = transport_->passkey();
  snprintf(code, sizeof(code), "%03u %03u", static_cast<unsigned>(key / 1000), static_cast<unsigned>(key % 1000));
  y += UITheme::drawCenteredWrappedText(renderer, area, UI_12_FONT_ID, y, code, 1, true, EpdFontFamily::BOLD);
  y += metrics.verticalSpacing * 2;
  UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, tr(STR_CLAUDE_BUDDY_PAIR_HINT), 3);
}

void ClaudeBuddyActivity::renderWaiting(const Rect& area) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int y = area.y + metrics.verticalSpacing;
  y += drawPet(Mood::Sleep, y) + metrics.verticalSpacing * 2;
  y += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, tr(STR_CLAUDE_BUDDY_WAITING), 2, true,
                                        EpdFontFamily::BOLD);
  y += metrics.verticalSpacing;
  y += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, transport_->advertisedName(), 1);
  y += metrics.verticalSpacing;
  UITheme::drawCenteredWrappedText(renderer, area, SMALL_FONT_ID, y, tr(STR_CLAUDE_BUDDY_SETUP), 4);
}

void ClaudeBuddyActivity::renderStatus(const Rect& area, const Mood mood, const int bottom) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const State& s = protocol_->state();
  int y = area.y;
  y += drawPet(mood, y) + metrics.verticalSpacing;
  y +=
      UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, moodLabel(mood), 1, true, EpdFontFamily::BOLD);

  char line[128];
  buildWho(s, line, sizeof(line));
  if (line[0] != '\0') {
    y += UITheme::drawCenteredWrappedText(renderer, area, SMALL_FONT_ID, y, line, 1);
  }
  y += metrics.verticalSpacing;

  snprintf(line, sizeof(line), "%s %u   %s %u   %s %u", tr(STR_CLAUDE_BUDDY_SESSIONS), s.total,
           tr(STR_CLAUDE_BUDDY_RUNNING), s.running, tr(STR_CLAUDE_BUDDY_WAITING_COUNT), s.waiting);
  y += UITheme::drawCenteredWrappedText(renderer, area, SMALL_FONT_ID, y, line, 1);
  y += metrics.verticalSpacing;

  if (s.msg[0] != '\0') {
    y += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, s.msg, 2);
    y += metrics.verticalSpacing;
  }

  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int footerY = bottom - lineH - metrics.verticalSpacing;
  const int listBottom = footerY - metrics.verticalSpacing;
  const int total = s.entryCount;
  int rows = std::max(0, (listBottom - y) / lineH);
  if (total > rows && rows > 0) --rows;
  entryRows_ = rows;
  const int first = std::clamp(entryScroll_, 0, std::max(0, total - rows));
  const int last = std::min(total, first + rows);
  for (int i = first; i < last; ++i) {
    const std::string text = renderer.truncatedText(SMALL_FONT_ID, s.entries[i], area.width);
    renderer.drawText(SMALL_FONT_ID, area.x, y + (i - first) * lineH, text.c_str());
  }
  if (total > rows && rows > 0) drawScrollIndicator(area, y + rows * lineH, first + 1, last, total);

  snprintf(line, sizeof(line), "%s %u   %s %u", tr(STR_CLAUDE_BUDDY_TOKENS_TODAY), s.tokensToday,
           tr(STR_CLAUDE_BUDDY_LEVEL), tokens_.level());
  UITheme::drawCenteredText(renderer, area, SMALL_FONT_ID, footerY, line);
}

void ClaudeBuddyActivity::renderReply(const Rect& area, const int bottom) {
  const State& s = protocol_->state();
  constexpr int fontId = UI_10_FONT_ID;
  const int lineH = renderer.getLineHeight(fontId);
  const int rows = std::max(1, (bottom - area.y) / lineH);
  replyRows_ = rows;

  if (s.reply[0] == '\0') {
    replyLineCount_ = 0;
    UITheme::drawCenteredWrappedText(renderer, area, fontId, area.y + area.height / 3, tr(STR_CLAUDE_BUDDY_NO_REPLY),
                                     2);
    return;
  }

  if (replyCacheVersion_ != s.replyVersion || replyCacheWidth_ != area.width || replyLines_.empty()) {
    replyLines_.clear();
    replyLines_.reserve(160);
    const char* p = s.reply;
    while (true) {
      const char* nl = strchr(p, '\n');
      const size_t n = nl ? static_cast<size_t>(nl - p) : strlen(p);
      if (n == 0) {
        replyLines_.emplace_back();
      } else {
        const std::string paragraph(p, n);
        for (auto& wrapped : renderer.wrappedText(fontId, paragraph.c_str(), area.width, kMaxParagraphLines)) {
          if (static_cast<int>(replyLines_.size()) >= kMaxReplyLines) break;
          replyLines_.push_back(std::move(wrapped));
        }
      }
      if (!nl || static_cast<int>(replyLines_.size()) >= kMaxReplyLines) break;
      p = nl + 1;
    }
    replyCacheVersion_ = s.replyVersion;
    replyCacheWidth_ = area.width;
  }

  const int total = static_cast<int>(replyLines_.size());
  replyLineCount_ = total;
  const int textRows = total > rows ? rows - 1 : rows;
  replyRows_ = textRows;
  const int first = std::clamp(replyScroll_, 0, std::max(0, total - textRows));
  const int last = std::min(total, first + textRows);
  for (int i = first; i < last; ++i) {
    renderer.drawText(fontId, area.x, area.y + (i - first) * lineH, replyLines_[i].c_str());
  }
  if (total > textRows) drawScrollIndicator(area, area.y + textRows * lineH, first + 1, last, total);
}

void ClaudeBuddyActivity::renderStats(const Rect& area, const int bottom) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const State& s = protocol_->state();
  constexpr int fontId = UI_10_FONT_ID;
  const int rowH = renderer.getLineHeight(fontId) + metrics.verticalSpacing;

  char median[16] = "-";
  if (velocity_.count() > 0) snprintf(median, sizeof(median), "%us", static_cast<unsigned>(velocity_.median()));
  char level[16];
  snprintf(level, sizeof(level), "%u", static_cast<unsigned>(tokens_.level()));
  char toNext[16];
  snprintf(toNext, sizeof(toNext), "%u",
           static_cast<unsigned>(kTokensPerLevel - static_cast<uint32_t>(tokens_.credited() % kTokensPerLevel)));
  char approved[16];
  snprintf(approved, sizeof(approved), "%u", static_cast<unsigned>(approvals_));
  char denied[16];
  snprintf(denied, sizeof(denied), "%u", static_cast<unsigned>(denials_));
  char today[16];
  snprintf(today, sizeof(today), "%u", static_cast<unsigned>(s.tokensToday));
  char session[16];
  snprintf(session, sizeof(session), "%u", static_cast<unsigned>(s.tokens));

  const char* link = !transport_->connected() ? tr(STR_CLAUDE_BUDDY_MOOD_SLEEP)
                     : transport_->secure()   ? tr(STR_CLAUDE_BUDDY_LINK_ENCRYPTED)
                                              : tr(STR_CLAUDE_BUDDY_LINK_OPEN);
  const struct {
    const char* label;
    const char* value;
  } rows[] = {{tr(STR_CLAUDE_BUDDY_STAT_APPROVED), approved},
              {tr(STR_CLAUDE_BUDDY_STAT_DENIED), denied},
              {tr(STR_CLAUDE_BUDDY_STAT_MEDIAN), median},
              {tr(STR_CLAUDE_BUDDY_STAT_LEVEL), level},
              {tr(STR_CLAUDE_BUDDY_STAT_NEXT_LEVEL), toNext},
              {tr(STR_CLAUDE_BUDDY_TOKENS_TODAY), today},
              {tr(STR_CLAUDE_BUDDY_STAT_TOKENS_SESSION), session},
              {tr(STR_CLAUDE_BUDDY_STAT_LINK), link},
              {tr(STR_CLAUDE_BUDDY_STAT_BLUETOOTH), transport_->advertisedName()}};

  int y = area.y + metrics.verticalSpacing;
  for (const auto& row : rows) {
    if (y + rowH > bottom) break;
    renderer.drawText(fontId, area.x, y, row.label);
    const int w = renderer.getTextWidth(fontId, row.value, EpdFontFamily::BOLD);
    renderer.drawText(fontId, area.x + area.width - w, y, row.value, true, EpdFontFamily::BOLD);
    y += rowH;
  }
}

void ClaudeBuddyActivity::renderOptions(const Rect& area, const Mood mood) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int y = area.y + metrics.verticalSpacing;
  y += drawPet(mood, y) + metrics.verticalSpacing;
  UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, I18N.get(kPetNames[species_]), 1, true,
                                   EpdFontFamily::BOLD);
}

void ClaudeBuddyActivity::renderApproval(const Rect& area, const Mood mood, const int bottom) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Prompt& prompt = protocol_->state().prompt;
  int y = area.y;
  y += drawPet(mood, y) + metrics.verticalSpacing;
  y += UITheme::drawCenteredWrappedText(renderer, area, UI_12_FONT_ID, y, prompt.tool, 1, true, EpdFontFamily::BOLD);
  y += metrics.verticalSpacing;
  const int hintLines = std::max(1, (bottom - y) / renderer.getLineHeight(UI_10_FONT_ID));
  UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, prompt.hint, std::min(hintLines, 6));
  if (decisionSent_) {
    UITheme::drawCenteredText(renderer, area, UI_10_FONT_ID, bottom - renderer.getLineHeight(UI_10_FONT_ID),
                              tr(STR_CLAUDE_BUDDY_SENT), true, EpdFontFamily::BOLD);
  }
}

void ClaudeBuddyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const uint32_t nowMs = millis();
  const Screen screen = currentScreen();
  const bool paged = isPaged(screen);
  const char* title = paged ? pageTitle(static_cast<int>(page_)) : tr(STR_CLAUDE_BUDDY);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::drawCompact(renderer, title);
  } else {
    CompactHeader::drawTitle(renderer, title);
  }

  const bool approvalOpen = screen == Screen::Approval && !decisionSent_;
  const bool optionsPage = paged && page_ == Page::Options;
  const bool showActions = approvalOpen || optionsPage;
  const uint8_t actionCount = approvalOpen ? 2 : kOptionCount;
  const int bottomEdge = paged ? pagedBottom(renderer) : approvalBottom(renderer);
  const auto actions = actionLayout(renderer, actionCount, bottomEdge);

  const int top = CompactHeader::contentTop(metrics) + metrics.verticalSpacing;
  const int contentBottom = showActions ? actions.container.y - metrics.verticalSpacing : bottomEdge;
  const Rect area{metrics.contentSidePadding, top, renderer.getScreenWidth() - metrics.contentSidePadding * 2,
                  std::max(1, contentBottom - top)};

  if (screen == Screen::Error) {
    UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, area.y + area.height / 3,
                                     tr(STR_CLAUDE_BUDDY_ERROR), 3, true, EpdFontFamily::BOLD);
  } else {
    const bool live = isLinkLive(protocol_->state(), transport_->connected(), nowMs);
    const Mood mood = deriveMood(protocol_->state(), live, timers_, nowMs);
    if (screen == Screen::Pairing) {
      renderPairing(area);
    } else if (screen == Screen::Approval) {
      renderApproval(area, mood, contentBottom);
    } else {
      switch (page_) {
        case Page::Status:
          if (screen == Screen::Waiting) {
            renderWaiting(area);
          } else {
            renderStatus(area, mood, contentBottom);
          }
          break;
        case Page::Reply:
          renderReply(area, contentBottom);
          break;
        case Page::Stats:
          renderStats(area, contentBottom);
          break;
        case Page::Options:
          renderOptions(area, mood);
          break;
      }
    }
  }

  const bool forgottenShown = forgottenAtMs_ != 0 && nowMs - forgottenAtMs_ < kForgottenShownMs;
  const char* forgetLabel = forgetArmed_     ? tr(STR_CLAUDE_BUDDY_FORGET_CONFIRM)
                            : forgottenShown ? tr(STR_CLAUDE_BUDDY_FORGOTTEN)
                                             : tr(STR_CLAUDE_BUDDY_FORGET);
  if (showActions) {
    if (approvalOpen) {
      const char* labels[] = {tr(STR_CLAUDE_BUDDY_APPROVE), tr(STR_CLAUDE_BUDDY_DENY)};
      TouchActionButtons::draw(renderer, actions, labels, 0, -1, UI_10_FONT_ID);
    } else {
      char petLabel[48];
      snprintf(petLabel, sizeof(petLabel), tr(STR_CLAUDE_BUDDY_PET), I18N.get(kPetNames[species_]));
      const char* labels[] = {petLabel, forgetLabel};
      TouchActionButtons::draw(renderer, actions, labels, -1, optionIndex_, UI_10_FONT_ID);
    }
  }
  if (paged) drawPageDots(renderer, static_cast<int>(page_), kPageCount);

  const auto hints =
      approvalOpen ? mappedInput.mapLabels(tr(STR_CLAUDE_BUDDY_DENY), tr(STR_CLAUDE_BUDDY_APPROVE), "", "")
      : paged      ? mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), optionsPage ? tr(STR_SELECT) : "",
                                           page_ != Page::Status ? mappedInput.withBackArrow(tr(STR_BACK)) : "",
                                           static_cast<int>(page_) < kPageCount - 1 ? tr(STR_NEXT) : "")
                   : mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4, true);

  HalDisplay::RefreshMode mode =
      screenTransitionRefresh_.modeFor(static_cast<uint8_t>(static_cast<int>(screen) * 8 + static_cast<int>(page_)));
  if (mode == HalDisplay::FAST_REFRESH && ++fastRefreshCount_ >= kFullRefreshEvery) mode = HalDisplay::HALF_REFRESH;
  if (mode != HalDisplay::FAST_REFRESH) fastRefreshCount_ = 0;
  renderer.displayBuffer(mode);
}

#endif
