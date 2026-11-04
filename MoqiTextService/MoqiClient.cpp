//
//	Copyright (C) 2015 - 2020 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
//
//	This library is free software; you can redistribute it and/or
//	modify it under the terms of the GNU Library General Public
//	License as published by the Free Software Foundation; either
//	version 2 of the License, or (at your option) any later version.
//
//	This library is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//	Library General Public License for more details.
//
//	You should have received a copy of the GNU Library General Public
//	License along with this library; if not, write to the
//	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//	Boston, MA  02110-1301, USA.
//

#include "MoqiClient.h"
#include "JsonCompat.h"
#include "KeyGuardLog.h"
#include "libIME2/src/Utils.h"
#include "libIME2/src/DebugLogConfig.h"
#include "../proto/ProtoFraming.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <json/json.h>

#include "MoqiTextService.h"
#include "MoqiImeModule.h"
#include <Shellapi.h>
#include <VersionHelpers.h> // Provided by Windows SDK >= 8.1
#include <Winnls.h> // for IS_HIGH_SURROGATE() macro for checking UTF16 surrogate pairs
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <exception>
#include <thread>

using namespace std;

namespace Moqi {

static constexpr UINT ASYNC_RPC_POLL_INTERVAL_MS = 50;
static constexpr int FIRST_PRINTABLE_KEY_RPC_WAIT_MS = 200;
// bug#13（2026-10-23）：应用激活后的首个可打印键若撞上激活/建连窗口，
// 旧的 200ms 超时会把键静默吃掉（"第一个字母丢失"）。激活路径本身有
// ~1s 的预算（见 Client::onActivate 的 ACTIVATION_CONNECT_ATTEMPTS），
// 首键等待与之对齐：宁可顿一下，也不丢字（对齐搜狗/微软拼音的行为）。
// 2026-10-30 提到 10s：1.5s 只盖住常规激活（实测 ~110ms），盖不住冷启动
// （启动器被拉起 + 后端 init/部署，实测可达 5~14s，开机首切/退出引擎后
// 首键都会走到）。预热让这个长等待极少发生，发生时宁可 UI 顿一下。
static constexpr int FIRST_PRINTABLE_KEY_ACTIVATION_WAIT_MS = 10000;
static constexpr DWORD RPC_BUSY_POLL_INTERVAL_MS = 5;
// Timeout for writing one request into the pipe. The 64KB pipe buffer accepts
// a request immediately unless the backend is completely stuck, so this only
// bounds a pathological case.
static constexpr DWORD RPC_WRITE_TIMEOUT_MS = 2000;

// Bounded waits so a not-yet-started or hung MoqiLauncher/backend can never
// block the TSF thread indefinitely:
// - RPC_REPLY_TIMEOUT_MS: max time to wait for a backend reply after sending a
//   request (replaces the unbounded blocking TransactNamedPipe). Kept generous
//   because the backend's first request per engine can take seconds at boot
//   (cold dictionary load).
// - RPC_ASYNC_REPLY_WAIT_MS: extra wait when draining a reply that arrived as
//   async notification(s) before the synchronous response.
// - CONNECT_ATTEMPT_TIMEOUT_MS * MAX_CONNECT_ATTEMPTS: total budget for
//   establishing the pipe connection on the key path.
// - ACTIVATION_CONNECT_ATTEMPTS: much smaller budget for the TSF activation
//   callback so Windows never sees a frozen text service at logon (which made
//   it fall back to another IME and persist that as the new default).
static constexpr DWORD RPC_REPLY_TIMEOUT_MS = 8000;
static constexpr DWORD RPC_ASYNC_REPLY_WAIT_MS = 2000;
static constexpr int CONNECT_ATTEMPT_TIMEOUT_MS = 500;
static constexpr int MAX_CONNECT_ATTEMPTS = 6;          // ~3s total
static constexpr int ACTIVATION_CONNECT_ATTEMPTS = 2;   // ~1s total
static constexpr ULONGLONG LAUNCHER_START_RETRY_COOLDOWN_MS = 3000;
static constexpr ULONGLONG LAUNCHER_KILL_RETRY_COOLDOWN_MS = 15000;

namespace {

bool isOrdinaryPrintableKey(Ime::KeyEvent &keyEvent) {
  const UINT charCode = keyEvent.charCode();
  if (charCode < 0x20) {
    return false;
  }
  if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
      (::GetKeyState(VK_MENU) & 0x8000) != 0) {
    return false;
  }
  return true;
}

} // namespace

// 放在匿名命名空间外：MoqiTextService.cpp 也要写按键异常日志（见 KeyGuardLog.h）。
std::wstring rpcGuardLogPath() {
  const wchar_t *localAppData = _wgetenv(L"LOCALAPPDATA");
  if (!localAppData || !*localAppData) {
    return L"";
  }
  // 独立于 tsf-debug.log（那是 trace 档全量日志）：这里只记按键被放行/异常
  // 这类低频事件，无条件常开——默认 error 档下问题复现时才留得下证据。
  return std::wstring(localAppData) + L"\\MoqiIM\\Log\\tsf-key-guard.log";
}

void appendRpcGuardLog(const std::wstring &message) {
  const std::wstring logPath = rpcGuardLogPath();
  if (logPath.empty()) {
    return;
  }

  SYSTEMTIME now{};
  ::GetLocalTime(&now);
  wchar_t timestamp[32] = {};
  swprintf_s(timestamp, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", now.wYear,
             now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
             now.wMilliseconds);

  std::wofstream stream(logPath, std::ios::app);
  if (!stream.is_open()) {
    return;
  }
  stream << L"[" << timestamp << "] " << message << L"\n";
}

void appendRpcTraceLog(const std::wstring &message) {
  if (!Ime::isDebugLoggingEnabled()) {
    return;
  }
  appendRpcGuardLog(message);
}

// 其余文件内辅助函数回到匿名命名空间（内部链接），只有上面的按键异常日志
// 需要 MoqiTextService.cpp 共用。
namespace {

class ScopedRpcInProgress {
public:
  explicit ScopedRpcInProgress(std::atomic<int> &counter) : counter_(counter) {
    counter_.fetch_add(1, std::memory_order_acq_rel);
  }

  ~ScopedRpcInProgress() {
    counter_.fetch_sub(1, std::memory_order_acq_rel);
  }

private:
  std::atomic<int> &counter_;
};

std::wstring quotePairLogPath() {
  const wchar_t *localAppData = _wgetenv(L"LOCALAPPDATA");
  if (!localAppData || !*localAppData) {
    return L"";
  }
  return std::wstring(localAppData) + L"\\MoqiIM\\Log\\quote-pair-debug.log";
}

void appendQuotePairLog(const std::wstring &message) {
  if (!Ime::isDebugLoggingEnabled()) {
    return;
  }
  const std::wstring logPath = quotePairLogPath();
  if (logPath.empty()) {
    return;
  }

  SYSTEMTIME now{};
  ::GetLocalTime(&now);
  wchar_t timestamp[32] = {};
  swprintf_s(timestamp, L"%04d-%02d-%02d %02d:%02d:%02d", now.wYear,
             now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

  std::wofstream stream(logPath, std::ios::app);
  if (!stream.is_open()) {
    return;
  }
  stream << L"[" << timestamp << L"] " << message << L"\n";
}

int utf16CursorFromCodePointCursor(const std::wstring &text, int cursor) {
  if (cursor <= 0) {
    return 0;
  }

  int utf16Cursor = 0;
  const int textLength = static_cast<int>(text.length());
  for (int i = 0; i < cursor && utf16Cursor < textLength; ++i) {
    if (IS_HIGH_SURROGATE(text[utf16Cursor]) && utf16Cursor + 1 < textLength) {
      utf16Cursor += 2;
    } else {
      ++utf16Cursor;
    }
  }
  return utf16Cursor;
}

std::wstring formatCodePoints(const std::wstring &text) {
  if (text.empty()) {
    return L"(empty)";
  }

  std::wostringstream stream;
  stream << std::uppercase << std::hex;
  for (size_t i = 0; i < text.size(); ++i) {
    if (i != 0) {
      stream << L" ";
    }
    stream << L"U+" << std::setw(4) << std::setfill(L'0')
           << static_cast<unsigned int>(text[i]);
  }
  return stream.str();
}

std::vector<AutoPairRuleState> defaultAutoPairRules() {
  return {
      {L"“", L"”"}, {L"‘", L"’"}, {L"【", L"】"}, {L"《", L"》"},
      {L"<", L">"}, {L"(", L")"}, {L"（", L"）"}, {L"「", L"」"},
  };
}

bool shouldAutoPairSymbol(const std::wstring &commitString,
                          const std::vector<AutoPairRuleState> &rules,
                          std::wstring &pairedString) {
  if (commitString.size() != 1) {
    return false;
  }

  for (const auto &rule : rules) {
    if (rule.open.size() != 1 || rule.close.size() != 1) {
      continue;
    }
    const wchar_t symbol = commitString[0];
    if (symbol != rule.open[0] && symbol != rule.close[0]) {
      continue;
    }
    pairedString = rule.open + rule.close;
    return true;
  }
  return false;
}

} // namespace

static const char* methodNameToJson(const char* methodName) {
  // proto3 JSON symbolic names; Go decodes via protojson.
  if (strcmp(methodName, "init") == 0)
    return "METHOD_INIT";
  if (strcmp(methodName, "onActivate") == 0)
    return "METHOD_ON_ACTIVATE";
  if (strcmp(methodName, "onDeactivate") == 0)
    return "METHOD_ON_DEACTIVATE";
  if (strcmp(methodName, "filterKeyDown") == 0)
    return "METHOD_FILTER_KEY_DOWN";
  if (strcmp(methodName, "onKeyDown") == 0)
    return "METHOD_ON_KEY_DOWN";
  if (strcmp(methodName, "filterKeyUp") == 0)
    return "METHOD_FILTER_KEY_UP";
  if (strcmp(methodName, "onKeyUp") == 0)
    return "METHOD_ON_KEY_UP";
  if (strcmp(methodName, "onPreservedKey") == 0)
    return "METHOD_ON_PRESERVED_KEY";
  if (strcmp(methodName, "onCommand") == 0)
    return "METHOD_ON_COMMAND";
  if (strcmp(methodName, "onMenu") == 0)
    return "METHOD_ON_MENU";
  if (strcmp(methodName, "onCompartmentChanged") == 0)
    return "METHOD_ON_COMPARTMENT_CHANGED";
  if (strcmp(methodName, "onKeyboardStatusChanged") == 0)
    return "METHOD_ON_KEYBOARD_STATUS_CHANGED";
  if (strcmp(methodName, "onCompositionTerminated") == 0)
    return "METHOD_ON_COMPOSITION_TERMINATED";
  if (strcmp(methodName, "highlightCandidate") == 0)
    return "METHOD_HIGHLIGHT_CANDIDATE";
  if (strcmp(methodName, "selectCandidate") == 0)
    return "METHOD_SELECT_CANDIDATE";
  if (strcmp(methodName, "changePage") == 0)
    return "METHOD_CHANGE_PAGE";
  return "METHOD_UNSPECIFIED";
}

static std::string uuidToString(const UUID &uuid) {
  std::string result;
  LPOLESTR buf = nullptr;
  if (SUCCEEDED(::StringFromCLSID(uuid, &buf))) {
    result = utf16ToUtf8(buf);
    ::CoTaskMemFree(buf);
    // convert GUID to lower case
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  }
  return result;
}

bool uuidFromString(const char *uuidStr, UUID &result) {
  std::wstring utf16UuidStr = utf8ToUtf16(uuidStr);
  return SUCCEEDED(CLSIDFromString(utf16UuidStr.c_str(), &result));
}

bool parseHexColor(const std::string &text, COLORREF &result) {
  std::string value = text;
  if (!value.empty() && value.front() == '#') {
    value.erase(value.begin());
  }
  if (value.size() != 6) {
    return false;
  }
  char *end = nullptr;
  const unsigned long rgb = std::strtoul(value.c_str(), &end, 16);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  result = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
  return true;
}

Client::Client(TextService *service, REFIID langProfileGuid)
    : textService_(service), guid_{uuidToString(langProfileGuid)},
      pipe_(INVALID_HANDLE_VALUE), rpcInProgress_(0),
      activationInProgress_(false), nextSeqNum_(0), isActivated_(false),
      shouldWaitConnection_{true}, handshakeComplete_{false},
      connectAttempts_{MAX_CONNECT_ATTEMPTS},
      lastLauncherStartAttemptTick_{0}, lastLauncherKillTick_{0},
      asyncPollTimerWindow_(nullptr),
      asyncPollTimerId_(0), asyncFlushInProgress_(false),
      autoPairRules_(defaultAutoPairRules()), localEngine_(service),
      keyboardHealAttempted_(false) {}

Client::~Client(void) {
  if (asyncPollTimerId_ != 0) {
    ::KillTimer(asyncPollTimerWindow_, asyncPollTimerId_);
    asyncPollTimerId_ = 0;
    asyncPollTimerWindow_ = nullptr;
  }
  // 从 TSF 注销本实例注册的语言栏按钮，再让 map 析构释放引用：
  // 只 Release 不 RemoveItem 的话 msctf 侧引用永不归还，每次
  // "打字×切走"泄漏一批按钮对象（2026-10-24 mem-audit 实测定位）。
  if (!buttons_.empty()) {
    for (auto &item : buttons_) {
      textService_->removeButton(item.second);
    }
    buttons_.clear();
  }
  closeRpcConnection();
  localEngine_.deactivate();
  resetTextServiceState();
  LangBarButton::clearIconCache();
}

// pack a keyEvent object into a json value
// static
void Client::addKeyEventToRpcRequest(Json::Value &request,
                                     Ime::KeyEvent &keyEvent) {
  Json::Value keyEvent2(Json::objectValue);
  keyEvent2["charCode"] = keyEvent.charCode();
  keyEvent2["keyCode"] = keyEvent.keyCode();
  keyEvent2["repeatCount"] = keyEvent.repeatCount();
  keyEvent2["scanCode"] = keyEvent.scanCode();
  keyEvent2["isExtended"] = keyEvent.isExtended();
  Json::Value states(Json::arrayValue);
  const BYTE *keyStates = keyEvent.keyStates();
  for (int i = 0; i < 256; ++i) {
    states.append(keyStates[i]);
  }
  keyEvent2["keyStates"] = states;
  request["keyEvent"] = keyEvent2;
}

bool Client::handleRpcResponse(Json::Value &msg, Ime::EditSession *session,
                               bool applyEngineState) {
  bool success = msg.get("success", false).asBool();
  if (success) {
    updateStatus(msg, session, applyEngineState);
  }
  return success;
}

void Client::updateUI(const Json::Value &data) {
  for (auto it = data.begin(); it != data.end(); ++it) {
    const char *name = it.memberName();
    const Json::Value &value = *it;
    if (value.isString() && strcmp(name, "candFontName") == 0) {
      wstring fontName = utf8ToUtf16(value.asCString());
      textService_->setCandFontName(fontName);
    } else if (value.isString() && strcmp(name, "candCommentFontName") == 0) {
      wstring fontName = utf8ToUtf16(value.asCString());
      textService_->setCandCommentFontName(fontName);
    } else if (value.isInt() && strcmp(name, "candFontSize") == 0) {
      textService_->setCandFontSize(value.asInt());
    } else if (value.isInt() && strcmp(name, "candCommentFontSize") == 0) {
      textService_->setCandCommentFontSize(value.asInt());
    } else if (value.isInt() && strcmp(name, "candPerRow") == 0) {
      textService_->setCandPerRow(value.asInt());
    } else if (value.isInt() && strcmp(name, "candSpacing") == 0) {
      textService_->setCandSpacing(value.asInt());
    } else if (value.isBool() && strcmp(name, "candUseCursor") == 0) {
      textService_->setCandUseCursor(value.asBool());
    } else if (value.isBool() && strcmp(name, "inlinePreedit") == 0) {
      textService_->setInlinePreedit(value.asBool());
    } else if (value.isBool() && strcmp(name, "autoPairQuotes") == 0) {
      textService_->setAutoPairQuotes(value.asBool());
    } else if (value.isBool() && strcmp(name, "semicolonSelectSecond") == 0) {
      localEngine_.setSemicolonSelectSecond(value.asBool());
    } else if (value.isArray() && strcmp(name, "autoPairRules") == 0) {
      std::vector<AutoPairRuleState> rules;
      rules.reserve(value.size());
      for (const auto &item : value) {
        const Json::Value &open = item["open"];
        const Json::Value &close = item["close"];
        if (!open.isString() || !close.isString()) {
          continue;
        }
        std::wstring openText = utf8ToUtf16(open.asCString());
        std::wstring closeText = utf8ToUtf16(close.asCString());
        if (openText.empty() || closeText.empty()) {
          continue;
        }
        rules.push_back({std::move(openText), std::move(closeText)});
      }
      autoPairRules_ = std::move(rules);
    } else if (value.isString() && strcmp(name, "candBackgroundColor") == 0) {
      COLORREF color = textService_->candBackgroundColor();
      if (parseHexColor(value.asCString(), color)) {
        textService_->setCandBackgroundColor(color);
      }
    } else if (value.isString() && strcmp(name, "candHighlightColor") == 0) {
      COLORREF color = textService_->candHighlightColor();
      if (parseHexColor(value.asCString(), color)) {
        textService_->setCandHighlightColor(color);
      }
    } else if (value.isString() && strcmp(name, "candTextColor") == 0) {
      COLORREF color = textService_->candTextColor();
      if (parseHexColor(value.asCString(), color)) {
        textService_->setCandTextColor(color);
      }
    } else if (value.isString() && strcmp(name, "candHighlightTextColor") == 0) {
      COLORREF color = textService_->candHighlightTextColor();
      if (parseHexColor(value.asCString(), color)) {
        textService_->setCandHighlightTextColor(color);
      }
    } else if (value.isString() && strcmp(name, "candCommentColor") == 0) {
      COLORREF color = textService_->candCommentColor();
      if (parseHexColor(value.asCString(), color)) {
        textService_->setCandCommentColor(color);
      }
    } else if (value.isString() && strcmp(name, "candCommentHighlightColor") == 0) {
      COLORREF color = textService_->candCommentHighlightColor();
      if (parseHexColor(value.asCString(), color)) {
        textService_->setCandCommentHighlightColor(color);
      }
    }
  }
  textService_->applyCandidateAppearanceNow();
  textService_->refreshCandidateAppearance();
}

void Client::updateSelectionKeys(Json::Value &msg) {
  // set sel keys before update candidates
  const auto &setSelKeysVal = msg["setSelKeys"];
  if (setSelKeysVal.isString()) {
    // keys used to select candidates
    std::wstring selKeys = utf8ToUtf16(setSelKeysVal.asCString());
    // 2026-11-01：EmitUnpopulated 后空串出现在每个响应里，空串不是"清空
    // 序号"指令（旧协议省略零值），只在非空时更新，否则候选序号标签消失。
    if (!selKeys.empty()) {
      appendRpcTraceLog(L"[setSelKeys] len=" + std::to_wstring(selKeys.size()) +
                        L" first=U+" +
                        std::to_wstring(static_cast<unsigned int>(selKeys[0])));
      textService_->setSelKeys(selKeys);
    }
  }
}

void Client::updateMessageWindow(Json::Value &msg, Ime::EditSession *session,
                                 bool &endComposition) {
  const auto &showMessageVal = msg["showMessage"];
  if (showMessageVal.isObject()) {
    const Json::Value &message = showMessageVal["message"];
    const Json::Value &duration = showMessageVal["duration"];
    if (message.isString() && duration.isInt()) {
      if (!textService_->isComposing()) {
        textService_->startComposition(session->context());
        endComposition = true;
      }
      textService_->showMessage(session, utf8ToUtf16(message.asCString()),
                                duration.asInt());
    }
  }

  // hide message
  const auto &hideMessageVal = msg["hideMessage"];
  if (hideMessageVal.isBool() && hideMessageVal.asBool()) {
    textService_->hideMessage();
  }
}

void Client::updateCommitString(Json::Value &msg, Ime::EditSession *session,
                                bool suppressTerminationNotification) {
  // handle comosition and commit strings
  const auto &commitStringVal = msg["commitString"];
  if (commitStringVal.isString()) {
    const std::wstring rawCommitString = utf8ToUtf16(commitStringVal.asCString());
    const bool autoPairQuotesEnabled = textService_->autoPairQuotes();
    std::wstring commitString = rawCommitString;
    std::wstring pairedCommitString;
    const bool isAutoPairSymbol =
        shouldAutoPairSymbol(rawCommitString, autoPairRules_, pairedCommitString);
    if (isAutoPairSymbol) {
      appendQuotePairLog(L"[updateCommitString] raw=" +
                         formatCodePoints(rawCommitString) + L" auto_pair_quotes=" +
                         (autoPairQuotesEnabled ? L"true" : L"false"));
    }

    const bool autoPairedSymbols = autoPairQuotesEnabled && isAutoPairSymbol;
    if (autoPairedSymbols) {
      commitString = pairedCommitString;
      appendQuotePairLog(L"[updateCommitString] paired=" +
                         formatCodePoints(commitString));
    }
    if (!commitString.empty()) {
      // 上屏走"清空组字 + 普通插入"（weasel#107 模式）：组字带文本定稿会被
      // XAML 宿主（便笺）整段重新选中（2026-10-23 用户实测，14 个输入法仅墨奇
      // 触发）；组字清空后结束，最终文本用 InsertTextAtSelection 插入并显式
      // 设置插入点，应用没有"组字定稿"事件可挑。自动配对符号的光标位置直接
      // 由插入时的 caretFromEnd 指定，不再需要延迟左方向键。
      if (textService_->isComposing()) {
        if (suppressTerminationNotification) {
          textService_->suppressNextCompositionTerminatedNotification();
        }
        textService_->endComposition(session->context(), /*clearText=*/true);
      }
      textService_->insertTextAtSelection(
          session->context(), commitString.c_str(),
          static_cast<int>(commitString.length()),
          autoPairedSymbols ? 1 : 0);
      // FIXME: update the position of candidate and message window when the
      // composition string is changed.
      if (textService_->hasCandidateWindow()) {
        textService_->updateCandidatesWindow(session);
      }
      if (textService_->messageWindow_ != nullptr) {
        textService_->updateMessageWindow(session);
      }
    }
  }
}

void Client::updateComposition(Json::Value &msg, Ime::EditSession *session,
                               bool &endComposition) {
  const auto &compositionStringVal = msg["compositionString"];
  bool emptyComposition = false;
  bool hasCompositionString = false;
  std::wstring compositionString;
  if (compositionStringVal.isString()) {
    // composition buffer
    compositionString = utf8ToUtf16(compositionStringVal.asCString());
    hasCompositionString = true;
    textService_->setCandidatePreedit(compositionString);
    if (!textService_->effectiveInlinePreedit()) {
      emptyComposition = compositionString.empty();
      if (!compositionString.empty()) {
        if (!textService_->isComposing()) {
          textService_->startComposition(session->context());
        }
        // Keep the TSF composition alive for key routing/anchor positioning,
        // but leave it visually empty because the preedit is rendered
        // externally in the candidate window.
        textService_->setCompositionString(session, L"", 0);
        textService_->showCandidates(session);
        textService_->updateCandidates(session);
      }
      if (textService_->hasCandidateWindow() && compositionString.empty()) {
        textService_->updateCandidatesWindow(session);
      }
      if (textService_->messageWindow_ != nullptr) {
        textService_->updateMessageWindow(session);
      }
      if (compositionString.empty() && textService_->isComposing() &&
          !textService_->showingCandidates()) {
        textService_->setCompositionString(session, L"", 0);
        endComposition = true;
      }
    } else {
      if (compositionString.empty()) {
        textService_->setCandidatePreedit(L"");
        emptyComposition = true;
        if (textService_->isComposing() && !textService_->showingCandidates()) {
          // when the composition buffer is empty and we are not showing the
          // candidate list, end composition.
          textService_->setCompositionString(session, L"", 0);
          endComposition = true;
        }
      } else {
        if (!textService_->isComposing()) {
          textService_->startComposition(session->context());
        }
        // LibIME2 setCompositionString() always collapses the selection to the
        // end of the composition. Skip it when only the caret moved (e.g. Tab
        // -> Shift+Right) so setCompositionCursor() is not fighting a stale
        // end-of-string selection in hosts that ignore cursor-only updates.
        bool compositionTextUnchanged = false;
        if (textService_->isComposing()) {
          compositionTextUnchanged =
              textService_->compositionString(session) == compositionString;
        }
        if (!compositionTextUnchanged) {
          textService_->setCompositionString(session, compositionString.c_str(),
                                             compositionString.length());
        }
      }
      // FIXME: update the position of candidate and message window when the
      // composition string is changed.
      if (textService_->hasCandidateWindow()) {
        textService_->updateCandidatesWindow(session);
      }
      if (textService_->messageWindow_ != nullptr) {
        textService_->updateMessageWindow(session);
      }
    }
  }

  const auto &compositionCursorVal = msg["compositionCursor"];
  if (compositionCursorVal.isInt()) {
    // composition cursor
    int compositionCursor = compositionCursorVal.asInt();
    if (!hasCompositionString) {
      compositionString = textService_->effectiveInlinePreedit()
                              ? textService_->compositionString(session)
                              : textService_->candidatePreedit();
    }
    const int fixedCursorPos =
        utf16CursorFromCodePointCursor(compositionString, compositionCursor);
    textService_->setCandidatePreeditCursor(fixedCursorPos);
    if (textService_->effectiveInlinePreedit() && !emptyComposition) {
      if (!textService_->isComposing()) {
        textService_->startComposition(session->context());
      }
      textService_->setCompositionCursor(session, fixedCursorPos);
    }
  }
  if (endComposition) {
    textService_->endComposition(session->context());
  }
}

void Client::updateLanguageButtons(Json::Value &msg) {
  // language buttons
  const auto &addButtonVal = msg["addButton"];
  if (addButtonVal.isArray()) {
    for (const auto &btn : addButtonVal) {
      // 后端 onActivate 与首键建会话会各发一次 addButton（同 id）：
      // 已注册的按钮就地更新即可，重复 AddItem 会多出一份无人释放的
      // msctf 引用（2026-10-24 实测每次打字×切换泄漏 5 个按钮对象）。
      const std::string addId = btn["id"].asString();
      auto existing = buttons_.find(addId);
      if (existing != buttons_.end()) {
        existing->second->updateFromJson(btn);
        continue;
      }
      auto langBtn = Ime::ComPtr<Moqi::LangBarButton>::takeover(
          Moqi::LangBarButton::fromJson(textService_, btn));
      if (langBtn != nullptr) {
        buttons_.emplace(addId, langBtn); // insert into the map
        textService_->addButton(langBtn);
        appendRpcGuardLog(L"[addButton] id=" + utf8ToUtf16(addId.c_str()) +
                          L" added to TSF");
      } else {
        appendRpcGuardLog(L"[addButton] id=" + utf8ToUtf16(addId.c_str()) +
                          L" fromJson returned NULL");
      }
    }
  }

  const auto &removeButtonVal = msg["removeButton"];
  if (removeButtonVal.isArray()) {
    // FIXME: handle windows-mode-icon
    for (const auto &btn : removeButtonVal) {
      if (btn.isString()) {
        string id = btn.asString();
        auto map_it = buttons_.find(id);
        if (map_it != buttons_.end()) {
          textService_->removeButton(map_it->second);
          buttons_.erase(map_it); // remove from the map
        }
      }
    }
  }
  const auto &changeButtonVal = msg["changeButton"];
  if (changeButtonVal.isArray()) {
    // FIXME: handle windows-mode-icon
    for (const auto &btn : changeButtonVal) {
      if (btn.isObject()) {
        string id = btn["id"].asString();
        auto map_it = buttons_.find(id);
        if (map_it != buttons_.end()) {
          map_it->second->updateFromJson(btn);
        }
      }
    }
  }
}

void Client::updatePreservedKeys(Json::Value &msg) {
  const auto &removePreservedKeyVal = msg["removePreservedKey"];
  if (removePreservedKeyVal.isArray()) {
    for (auto &item : removePreservedKeyVal) {
      if (item.isString()) {
        UUID guid = {0};
        if (uuidFromString(item.asCString(), guid)) {
          textService_->removePreservedKey(guid);
        }
      }
    }
  }

  const auto &addPreservedKeyVal = msg["addPreservedKey"];
  if (addPreservedKeyVal.isArray()) {
    // preserved keys
    for (auto &key : addPreservedKeyVal) {
      if (key.isObject()) {
        UINT keyCode = key["keyCode"].asUInt();
        UINT modifiers = key["modifiers"].asUInt();
        UUID guid = {0};
        if (uuidFromString(key["guid"].asCString(), guid)) {
          textService_->addPreservedKey(keyCode, modifiers, guid);
        }
      }
    }
  }
}

void Client::updateKeyboardStatus(Json::Value &msg) {
  // 2026-11-01 事故修复：EmitUnpopulated 后 openKeyboard:false 出现在每个
  // 响应里（Go 侧从未把它当"关闭"指令用，false 只是零值）——旧协议里 false
  // 被省略、DLL 只在收到 true 时开键盘。这里恢复"只在 true 时动作"语义，
  // 否则每条 RPC 回复都会把键盘关掉（x86 应用全裸英文 + 重连风暴根因）。
  const auto &openKeyboardVal = msg["openKeyboard"];
  if (openKeyboardVal.isBool() && openKeyboardVal.asBool()) {
    textService_->setKeyboardOpen(true);
  }
}

void Client::updateStatus(Json::Value &msg, Ime::EditSession *session,
                          bool applyEngineState) {
  // 云剪贴板退出标志（cloudClip:false）在同步回复里（commit/Esc/删除），
  // 此前只有 applyAsyncResponse 处理，导致进入转发模式后永远退不回来。
  if (msg.isMember("cloudClip")) {
    cloudClipRpcMode_ = msg["cloudClip"].asBool();
    appendRpcGuardLog(L"[cloudclip] rpc mode " +
                      std::wstring(cloudClipRpcMode_ ? L"ON" : L"OFF"));
  }
  // 2026-11-02 审计结论：protojson EmitUnpopulated 让隐式零值字段出现在每条
  // 响应里（compositionString:""/candidateList:[]/showCandidates:false…）。
  // 只有按键类方法（filterKeyDown/onKeyDown/onKeyUp/filterKeyUp/onPreservedKey
  // 及后端异步推送）的响应才是引擎状态快照，空值 = "引擎已清空"的合法指令；
  // 辅助方法（init/onActivate/onCommand/onMenu/compartment/keyboard）的响应
  // 只是控制回执，其零值状态字段必须忽略，否则每条回执都会清组字/清候选
  // （proto 时代靠"字段缺省"天然免疫，JSON 迁移后靠这个门控显式恢复）。
  if (applyEngineState) {
    updateSelectionKeys(msg);
  }
  bool endComposition = false;
  const Json::Value customizeUIVal = msg.get("customizeUi", msg["customizeUI"]);
  if (customizeUIVal.isObject()) {
    updateUI(customizeUIVal);
  }

  if (session != nullptr) { // if an edit session is available
    updateMessageWindow(msg, session, endComposition);

    if (applyEngineState) {
      const bool hasCommitString =
          msg["commitString"].isString() &&
          !utf8ToUtf16(msg["commitString"].asCString()).empty();
      const bool hasNonEmptyComposition =
          msg["compositionString"].isString() &&
          !utf8ToUtf16(msg["compositionString"].asCString()).empty();

      // Fixed-length schemas may commit current code and immediately start the
      // next composition in the same response, e.g. "ggtts" -> commit "五笔"
      // while leaving "s" active. In that case, update the new composition
      // first, then bind the candidate list to that new composition.
      if (hasCommitString && hasNonEmptyComposition) {
        updateCommitString(msg, session, true);
        updateComposition(msg, session, endComposition);
        updateCandidateList(msg, session);
      } else {
        updateCandidateList(msg, session);
        updateCommitString(msg, session, false);
        updateComposition(msg, session, endComposition);
      }
    }
  }

  updateLanguageButtons(msg);

  // preserved keys
  updatePreservedKeys(msg);

  // keyboard status
  updateKeyboardStatus(msg);

}

void Client::updateCandidateList(Json::Value &msg, Ime::EditSession *session) {
  // handle candidate list
  const auto &showCandidatesVal = msg["showCandidates"];
  const bool hasExplicitShowCandidates = showCandidatesVal.isBool();
  const bool explicitShowCandidates =
      hasExplicitShowCandidates && showCandidatesVal.asBool();

  const auto &candidateListVal = msg["candidateList"];
  const auto &candidateEntriesVal = msg["candidateEntries"];
  bool hasCandidatePayload = false;
  bool hasVisibleCandidates = false;

  if (candidateEntriesVal.isArray()) {
    hasCandidatePayload = true;
    vector<CandidateUiItem> &candidates = textService_->candidates_;
    candidates.clear();
    for (const auto &candidate : candidateEntriesVal) {
      CandidateUiItem item;
      if (candidate.isObject()) {
        if (candidate["text"].isString()) {
          item.text = utf8ToUtf16(candidate["text"].asCString());
        }
        if (candidate["comment"].isString()) {
          item.comment = utf8ToUtf16(candidate["comment"].asCString());
        }
      }
      candidates.emplace_back(std::move(item));
    }
    hasVisibleCandidates = !candidates.empty();
  } else if (candidateListVal.isArray()) {
    hasCandidatePayload = true;
    // handle candidates
    // FIXME: directly access private member is dirty!!!
    vector<CandidateUiItem> &candidates = textService_->candidates_;
    candidates.clear();
    for (const auto &candidate : candidateListVal) {
      CandidateUiItem item;
      item.text = utf8ToUtf16(candidate.asCString());
      candidates.emplace_back(std::move(item));
    }
    hasVisibleCandidates = !candidates.empty();
  }

  const bool hasCandidateRecoveryState =
      textService_->pendingCandidateRecovery() || textService_->showingCandidates() ||
      textService_->isComposing() || !textService_->candidatePreedit().empty();
  const bool shouldRestoreCandidates =
      hasVisibleCandidates && hasCandidateRecoveryState &&
      (!hasExplicitShowCandidates || !explicitShowCandidates);
  const bool shouldShowCandidates = explicitShowCandidates || shouldRestoreCandidates;

  if (shouldShowCandidates) {
    // start composition if we are not composing.
    // this is required to correctly position the candidate window
    if (!textService_->isComposing()) {
      textService_->startComposition(session->context());
    }
    textService_->showCandidates(session);
  }

  if (hasCandidatePayload) {
    textService_->updateCandidates(session);
    if (!hasVisibleCandidates ||
        (hasExplicitShowCandidates && !explicitShowCandidates && !shouldRestoreCandidates)) {
      textService_->hideCandidates();
    }
  } else if (hasExplicitShowCandidates && !explicitShowCandidates) {
    textService_->hideCandidates();
  }

  const auto &candidateCursorVal = msg["candidateCursor"];
  if (candidateCursorVal.isInt()) {
    if (textService_->hasCandidateWindow()) {
      if (textService_->setCandidateCursor(candidateCursorVal.asInt())) {
        textService_->refreshCandidates();
      }
    }
  }
}

// handlers for the text service

// Kick the process-wide rime runtime init (background thread; never blocks
// the TSF callback). Idempotent, cheap, and a no-op on x86 or when
// MOQI_LOCAL_ENGINE=0.
void Client::requestLocalEngineInit(TextService *service) {
  RimeRuntime &runtime = RimeRuntime::instance();
  if (!runtime.supported() || runtime.ready()) {
    return;
  }
  // Failed 可重试（requestInit 内部复位 Failed→Idle）；5 秒退避。
  static std::atomic<ULONGLONG> lastAttempt{0};
  const ULONGLONG now = ::GetTickCount64();
  const ULONGLONG prev = lastAttempt.load();
  if (runtime.failed() && now - prev < 5000) {
    return;
  }
  lastAttempt.store(now);
  auto module =
      static_cast<Moqi::ImeModule *>(service->imeModule().operator->());
  if (module == nullptr || module->programDir().empty() ||
      module->backendDirs().empty()) {
    return;
  }
  runtime.requestInit(module->programDir(), module->backendDirs().front());
}

// Try to take over the key path with the in-process engine. Only when the
// runtime is ready and no composition/candidates/async responses are in
// flight, so the RPC session never hands over half-finished state.
bool Client::maybeEngageLocalEngine() {
  RimeRuntime &runtime = RimeRuntime::instance();
  if (!runtime.supported() || localEngine_.active() ||
      !pendingAsyncResponses_.empty()) {
    return false;
  }
  return localEngine_.tryEngage();
}

void Client::onActivate() {
  activationInProgress_.store(true, std::memory_order_release);
  // Use a much smaller connection budget during TSF activation. At logon the
  // system activates the default input method very early; blocking the TSF
  // callback for tens of seconds made Windows consider Moqi dead and fall back
  // to another IME (and remember that fallback as the new default). With a ~1s
  // budget the activation always returns promptly; if the launcher is not up
  // yet the connection (and this onActivate RPC) is completed lazily by
  // waitForRpcConnection() on the first key press instead.
  connectAttempts_ = ACTIVATION_CONNECT_ATTEMPTS;
  auto req = createRpcRequest("onActivate");
  req["isKeyboardOpen"] = textService_->isKeyboardOpened();

  Json::Value ret;
  callRpcMethod(req, ret);
  if (handleRpcResponse(ret, nullptr, false)) {
  }
  activationInProgress_.store(false, std::memory_order_release);
  isActivated_ = true;
  connectAttempts_ = MAX_CONNECT_ATTEMPTS;
  // 2026-11-01 键盘开关自愈（每进程一次）：关闭态会持久化到输入配置，
  // 新进程继承后按键不进入本服务（直出英文）。风暴根因（openKeyboard:false
  // 逐响应关键盘）已修，单次校正不会再触发翻转循环。
  if (!keyboardHealAttempted_) {
    keyboardHealAttempted_ = true;
    if (!textService_->isKeyboardOpened()) {
      textService_->setKeyboardOpen(true);
    }
  }
  // 阶段1：激活即后台初始化进程内引擎，首个空闲按键点完成接管。
  requestLocalEngineInit(textService_);
  // 输入状态共享/方案跟随（Go：各实例激活时读文件重放）。本地引擎已接管
  // 的进程在此重读，切换应用即跟随其他应用的共享状态。
  if (localEngine_.active()) {
    localEngine_.refreshSyncedSettings();
  }
}

void Client::onDeactivate() {
  // Fire-and-forget: never block the app's TSF thread waiting for a reply to
  // a notification (e.g. when the backend is hung at boot).
  sendRpcNoWait("onDeactivate");
  const bool userDictDirty =
      localEngine_.active() && localEngine_.consumeUserDictDirty();
  localEngine_.deactivate();
  LangBarButton::clearIconCache();
  isActivated_ = false;
  if (userDictDirty) {
    // 真反激活：会话已销毁=安全窗口，后台导出学习快照（无组字可查）。
    // force：进程可能马上退出，必须跳过 20 秒防抖（否则最后一次学习丢失）。
    scheduleUserDbExport(/*checkComposition=*/false, /*requireDirty=*/false,
                         /*force=*/true);
  }
}

// 用户词典学习闭环（2026-11-02）：后台线程导出（维护任务 ~百毫秒级，绝不
// 在 TSF 线程做）。checkComposition=true 时先查无组字再导（RimeSyncUserData
// 会清会话组字）。20 秒防抖 + 在途互斥；有学习才动作。
void Client::scheduleUserDbExport(bool checkComposition, bool requireDirty,
                                  bool force) {
  // 反激活后 active() 已为 false（会话销毁），但导出是进程级 userdb 操作，
  // 仍可执行——这正是进程退出前最后一次导出的路径。非 force 的常规路径
  // 仍要求引擎已接管。
  if (!force && !localEngine_.active()) {
    return;
  }
  if (force && !localEngine_.canExportUserDb()) {
    return;
  }
  if (requireDirty && !localEngine_.hasUserDictChanges()) {
    return;
  }
  const ULONGLONG now = ::GetTickCount64();
  if (!force && now - lastUserDbExportTick_ < 20000) {
    return;
  }
  if (userDbExportInFlight_.exchange(true)) {
    return;
  }
  lastUserDbExportTick_ = now;
  std::thread([this, checkComposition]() {
    bool exported = false;
    if (!checkComposition || !localEngine_.hasComposition()) {
      // 导出+原子重建会话（RimeSyncUserData 会销毁全部会话，见实现注释）。
      exported = localEngine_.syncUserDbNow();
    }
    userDbExportInFlight_.store(false);
    if (exported) {
      sendRpcNoWaitReliable("userdbChanged");
    }
  }).detach();
}

bool Client::waitForRpcIdleForFirstKey() const {
  // 首键专用等待：普通 RPC 忙等 200ms；一旦看到激活/建连进行中，
  // 放宽到激活预算（FIRST_PRINTABLE_KEY_ACTIVATION_WAIT_MS）。
  const ULONGLONG start = ::GetTickCount64();
  ULONGLONG deadline = start + static_cast<ULONGLONG>(FIRST_PRINTABLE_KEY_RPC_WAIT_MS);
  for (;;) {
    const bool activation =
        activationInProgress_.load(std::memory_order_acquire);
    if (activation) {
      deadline =
          start + static_cast<ULONGLONG>(FIRST_PRINTABLE_KEY_ACTIVATION_WAIT_MS);
    }
    if (!activation && rpcInProgress_.load(std::memory_order_acquire) == 0) {
      return true;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      return false;
    }
    const DWORD sleepMs = static_cast<DWORD>(
        (std::min)(static_cast<ULONGLONG>(RPC_BUSY_POLL_INTERVAL_MS),
                   deadline - now));
    ::Sleep(sleepMs == 0 ? 1 : sleepMs);
  }
}

bool Client::filterKeyDown(Ime::KeyEvent &keyEvent) {
  // 阶段1：进程内引擎优先；接管点要求无组字/无候选/无在途异步响应。
  // 云剪贴板列表激活期间走 RPC（本地引擎会吃掉选字键刷掉列表）。
  // 引擎还在首次部署（全新用户目录 1.5-5 秒）时，可打印键先等它就绪再判：
  // 直接放行会变成"刚装完第一句拼音直落英文"（2026-11-03 实报）。
  if (isOrdinaryPrintableKey(keyEvent)) {
    RimeRuntime &runtime = RimeRuntime::instance();
    if (runtime.supported() && !localEngine_.active() &&
        !runtime.ready() && !runtime.failed()) {
      const ULONGLONG waitStart = ::GetTickCount64();
      runtime.waitUntilReady(5000);
      appendRpcGuardLog(L"[filterKeyDown] waited for engine " +
                        std::to_wstring(::GetTickCount64() - waitStart) + L"ms");
    }
  }
  maybeEngageLocalEngine();
  if (localEngine_.active() && !cloudClipRpcMode_) {
    Json::Value ret;
    if (localEngine_.filterKeyDown(keyEvent, ret)) {
      ret["success"] = true;
      handleRpcResponse(ret, nullptr, true);
      const bool consumed = ret["returnValue"].asBool();
      const BYTE *ks = keyEvent.keyStates();
      const bool hasCtrlOrAlt =
          ks != nullptr &&
          (((ks[VK_CONTROL] & 0x80) != 0) || ((ks[VK_MENU] & 0x80) != 0));
      if (consumed || !hasCtrlOrAlt) {
        return consumed;
      }
      // 本地引擎未消费的 Ctrl/Alt 组合键转发 daemon 判定（AI 热键需要组字、
      // 云剪贴板 Ctrl+0 无需组字、其他组合键 daemon 返回 0 交还宿主）。
      // 普通字母键不走这条路——打字热路径零 RPC 不动摇。
      // daemon 的 rime session 独立于本进程，组字/候选随请求带上（AI 用）。
      Json::Value state;
      localEngine_.getCompositionState(state);
      auto req = createRpcRequest("filterKeyDown");
      addKeyEventToRpcRequest(req, keyEvent);
      req["localComposition"] = state["composition"];
      req["localCandidates"] = state["candidates"];
      Json::Value aiRet;
      if (!callRpcMethod(req, aiRet) ||
          !handleRpcResponse(aiRet, nullptr, false)) {
        return false;
      }
      const bool aiConsumed = aiRet["returnValue"].asBool();
      if (aiConsumed) {
        // 结果经推送帧回来，候选窗 WM_TIMER 轮询取回（见 applyAsyncResponse）。
        refreshAsyncPollTimer();
      }
      appendRpcGuardLog(L"[filterKeyDown] chord -> daemon vk=" +
                        std::to_wstring(keyEvent.keyCode()) +
                        (aiConsumed ? L" ai=consumed" : L" ai=passed"));
      return aiConsumed;
    }
    // 本地引擎失败（UWP/AppContainer 里常见：用户目录不可写/初始化失败）
    // 不能把键丢掉，落到下面的 daemon RPC 路径；32 位后端不依赖包内权限。
    appendRpcGuardLog(
        L"[filterKeyDown] local engine failed; falling back to daemon");
  }
  if (isOrdinaryPrintableKey(keyEvent) &&
      !waitForRpcIdleForFirstKey()) {
    // 等不到空闲也不能吃键（2026-10-30 用户定调"必须解决，别家输入法从不丢"）：
    // 放行直通，字母以英文上屏——可见、可删，好过凭空消失。
    std::wostringstream log;
    log << L"[filterKeyDown] busy timeout; pass printable key through"
        << L" vk=" << keyEvent.keyCode()
        << L" char=" << keyEvent.charCode()
        << L" waited_ms="
        << (activationInProgress_.load(std::memory_order_acquire)
                ? FIRST_PRINTABLE_KEY_ACTIVATION_WAIT_MS
                : FIRST_PRINTABLE_KEY_RPC_WAIT_MS)
        << L" activation_in_progress="
        << (activationInProgress_.load(std::memory_order_acquire) ? L"true" : L"false")
        << L" rpc_in_progress="
        << rpcInProgress_.load(std::memory_order_acquire);
    appendRpcGuardLog(log.str());
    return false;
  }

  auto req = createRpcRequest("filterKeyDown");
  addKeyEventToRpcRequest(req, keyEvent);

  Json::Value ret;
  const bool rpcOk = callRpcMethod(req, ret);
  if (rpcOk && handleRpcResponse(ret, nullptr, true)) {
    return ret["returnValue"].asBool();
  }
  if (isOrdinaryPrintableKey(keyEvent)) {
    // RPC 硬失败（管道断/连接耗尽/应答坏）同样不吃键：直通，留痕。
    std::wostringstream log;
    log << L"[filterKeyDown] rpc failed; pass printable key through"
        << L" vk=" << keyEvent.keyCode()
        << L" char=" << keyEvent.charCode()
        << L" rpc_ok=" << (rpcOk ? L"true" : L"false");
    appendRpcGuardLog(log.str());
    return false;
  }
  return false;
}

bool Client::onKeyDown(Ime::KeyEvent &keyEvent, Ime::EditSession *session) {
  if (localEngine_.active() && !cloudClipRpcMode_) {
    Json::Value ret;
    if (localEngine_.onKeyDown(keyEvent, ret)) {
      ret["success"] = true;
      handleRpcResponse(ret, session, true);
      // 提交即有学习：安全时机（刚上屏，组字已清）后台导出（内部 20s 防抖）。
      if (ret.isMember("commitString")) {
        scheduleUserDbExport(/*checkComposition=*/true);
      }
      return ret["returnValue"].asBool();
    }
    if (isOrdinaryPrintableKey(keyEvent)) {
      appendRpcGuardLog(L"[onKeyDown] local engine failed after filter "
                        L"consumed the key; key lost vk=" +
                        std::to_wstring(keyEvent.keyCode()));
    }
    return false;
  }
  auto req = createRpcRequest("onKeyDown");
  addKeyEventToRpcRequest(req, keyEvent);

  Json::Value ret;
  callRpcMethod(req, ret);
  if (handleRpcResponse(ret, session, true)) {
    refreshAsyncPollTimer();
    flushPendingAsyncResponses(session);
    return ret["returnValue"].asBool();
  }
  // 键已被 filterKeyDown 消费，这里失败即丢字，无法补救（拿不到可靠的直通机会），
  // 至少无条件留痕，配合 tsf-key-guard.log 定位。
  if (isOrdinaryPrintableKey(keyEvent)) {
    appendRpcGuardLog(L"[onKeyDown] rpc failed after filter consumed the key; "
                      L"key lost vk=" + std::to_wstring(keyEvent.keyCode()));
  }
  return false;
}

bool Client::filterKeyUp(Ime::KeyEvent &keyEvent) {
  if (localEngine_.active()) {
    Json::Value ret;
    if (localEngine_.filterKeyUp(keyEvent, ret)) {
      ret["success"] = true;
      handleRpcResponse(ret, nullptr, true);
      return ret["returnValue"].asBool();
    }
    return false;
  }
  auto req = createRpcRequest("filterKeyUp");
  addKeyEventToRpcRequest(req, keyEvent);

  Json::Value ret;
  callRpcMethod(req, ret);
  if (handleRpcResponse(ret, nullptr, true)) {
    return ret["returnValue"].asBool();
  }
  return false;
}

bool Client::onKeyUp(Ime::KeyEvent &keyEvent, Ime::EditSession *session) {
  if (localEngine_.active()) {
    Json::Value ret;
  if (localEngine_.onKeyUp(keyEvent, ret)) {
    ret["success"] = true;
    handleRpcResponse(ret, session, true);
      return ret["returnValue"].asBool();
    }
    return false;
  }
  auto req = createRpcRequest("onKeyUp");
  addKeyEventToRpcRequest(req, keyEvent);

  Json::Value ret;
  callRpcMethod(req, ret);
  if (handleRpcResponse(ret, session, true)) {
    refreshAsyncPollTimer();
    flushPendingAsyncResponses(session);
    return ret["returnValue"].asBool();
  }
  return false;
}

bool Client::highlightCandidate(int index) {
  if (localEngine_.active() && !cloudClipRpcMode_) {
    Json::Value ret;
    if (localEngine_.highlightCandidate(index, ret)) {
      ret["success"] = true;
      return applyAsyncResponse(ret);
    }
    return false;
  }
  auto req = createRpcRequest("highlightCandidate");
  req["candidateIndex"] = index;

  Json::Value ret;
  if (!callRpcMethod(req, ret)) {
    return false;
  }
  return applyAsyncResponse(ret);
}

bool Client::selectCandidate(int index) {
  if (localEngine_.active() && !cloudClipRpcMode_) {
    Json::Value ret;
    if (localEngine_.selectCandidate(index, ret)) {
      ret["success"] = true;
      return applyAsyncResponse(ret);
    }
    return false;
  }
  auto req = createRpcRequest("selectCandidate");
  req["candidateIndex"] = index;

  Json::Value ret;
  if (!callRpcMethod(req, ret)) {
    return false;
  }
  return applyAsyncResponse(ret);
}

bool Client::changePage(bool backward) {
  if (localEngine_.active() && !cloudClipRpcMode_) {
    Json::Value ret;
    if (localEngine_.changePage(backward, ret)) {
      ret["success"] = true;
      return applyAsyncResponse(ret);
    }
    return false;
  }
  auto req = createRpcRequest("changePage");
  req["pageBackward"] = backward;

  Json::Value ret;
  if (!callRpcMethod(req, ret)) {
    return false;
  }
  return applyAsyncResponse(ret);
}

bool Client::onPreservedKey(const GUID &guid) {
  return onPreservedKey(guid, nullptr);
}

bool Client::onPreservedKey(const GUID &guid, Ime::EditSession *session) {
  auto guidStr = uuidToString(guid);
  if (!guidStr.empty()) {
    auto req = createRpcRequest("onPreservedKey");
    req["preservedKeyGuid"] = guidStr;

    Json::Value ret;
    callRpcMethod(req, ret);
    if (session != nullptr ? handleRpcResponse(ret, session, true)
                         : handleRpcResponse(ret, nullptr, true)) {
      return ret["returnValue"].asBool();
    }
  }
  return false;
}

bool Client::onCommand(UINT id, Ime::TextService::CommandType type) {
  appendRpcGuardLog(L"[onCommand] id=" + std::to_wstring(id) +
                    L" type=" + std::to_wstring(static_cast<int>(type)) +
                    L" local=" + (localEngine_.active() ? L"1" : L"0"));
  // 本地引擎接管时，改引擎状态的命令必须在本地执行（后端切换的是它自己
  // 会话的状态，本地引擎毫发无损——"图标变了打字没变"的根因）。
  if (localEngine_.active()) {
    Json::Value ret(Json::objectValue);
    ret["success"] = true;
    ret["seqNum"] = static_cast<Json::UInt>(nextSeqNum_++);
    ret["returnValue"] = 1;
    if ((id == 1 || id == 2) && localEngine_.toggleOption("ascii_mode", ret)) {
      handleRpcResponse(ret, nullptr, false);
      return true;
    }
    if (id == 3 && localEngine_.toggleOption("full_shape", ret)) {
      handleRpcResponse(ret, nullptr, false);
      return true;
    }
    if (id == 4 && localEngine_.toggleOption("ascii_punct", ret)) {
      handleRpcResponse(ret, nullptr, false);
      return true;
    }
    if (id == 5 && localEngine_.toggleOption("traditionalization", ret)) {
      handleRpcResponse(ret, nullptr, false);
      return true;
    }
    // 菜单开关区（2000+）：daemon 在菜单树里带了 "option"（选项名），
    // 本地直接翻转；树过期拿不到名字时落 RPC 兜底。
    if (id >= 2000 && id < 3000) {
      std::string optionName;
      for (const auto &it : lastMenuItems_) {
        if (it.get("id", -1).asInt() == static_cast<int>(id)) {
          optionName = it.get("option", "").asString();
          break;
        }
      }
      if (!optionName.empty() &&
          localEngine_.toggleOption(optionName.c_str(), ret)) {
        handleRpcResponse(ret, nullptr, false);
        return true;
      }
    }
    // 其余命令（方案/主题/维护等）仍走后端。
  }
  auto req = createRpcRequest("onCommand");
  req["commandId"] = id;
  req["commandType"] = static_cast<int>(type);

  Json::Value ret;
  callRpcMethod(req, ret);
  if (handleRpcResponse(ret, nullptr, false)) {
    // daemon 侧改了方案/共享态：本地引擎立即跟随（不必等下次激活）。
    if (localEngine_.active()) {
      // 总候选数随 customizeUi 下发：进程内引擎即时改 page size，
      // 否则用户要重启应用才见效（2026-11-03 用户实报）。
      const Json::Value &ui = ret["customizeUi"];
      if (ui.isObject() && ui["candidateCount"].isInt()) {
        localEngine_.applyCandidateCount(ui["candidateCount"].asInt());
      }
      localEngine_.refreshSyncedSettings();
    }
    return ret["returnValue"].asBool();
  }
  return false;
}

bool Client::sendOnMenu(std::string button_id, Json::Value &result) {
  appendRpcGuardLog(L"[onMenu] button=" + utf8ToUtf16(button_id.c_str()) +
                    L" wait=" + std::to_wstring(shouldWaitConnection_ ? 1 : 0) +
                    L" pipe=" +
                    std::to_wstring(pipe_ != INVALID_HANDLE_VALUE ? 1 : 0));
  auto req = createRpcRequest("onMenu");
  req["buttonId"] = button_id;

  const bool rpcOk = callRpcMethod(req, result);
  appendRpcGuardLog(L"[onMenuRpc] ok=" + std::to_wstring(rpcOk ? 1 : 0) +
                    L" pipe=" +
                    std::to_wstring(pipe_ != INVALID_HANDLE_VALUE ? 1 : 0));
  {
    const Json::Value& mi = result["menuItems"];
    if (mi.isArray()) {
      // 留存菜单树：onCommand 开关区（2000+）据此映射 id -> 选项名。
      lastMenuItems_ = mi;
    }
    std::wstring dump = L"[onMenuResult] isArray=" +
                        std::to_wstring(mi.isArray() ? 1 : 0) +
                        L" size=" + std::to_wstring(mi.size());
    if (mi.isArray()) {
      int shown = 0;
      for (const auto& item : mi) {
        if (shown >= 6) break;
        dump += L" {" + std::to_wstring(item.get("id", 0).asUInt()) + L":" +
                utf8ToUtf16(item.get("text", "").asCString()) + L"}";
        ++shown;
      }
    }
    appendRpcGuardLog(dump);
  }
  if (handleRpcResponse(result, nullptr, false)) {
    return true;
  }
  return false;
}

static void addMenuNodesToTfMenu(
    ITfMenu *pMenu, const std::vector<ProtocolJson::MenuNode> &nodes) {
  for (const auto &node : nodes) {
    const std::wstring text = utf8ToUtf16(node.text.c_str());
    DWORD flags = 0;
    ITfMenu *submenu = nullptr;
    if (node.separator) {
      flags = TF_LBMENUF_SEPARATOR;
    } else {
      if (node.checked)
        flags |= TF_LBMENUF_CHECKED;
      if (!node.enabled)
        flags |= TF_LBMENUF_GRAYED;
      if (!node.children.empty())
        flags |= TF_LBMENUF_SUBMENU;
    }
    pMenu->AddMenuItem(node.id, flags, NULL, NULL, text.c_str(), text.length(),
                       flags & TF_LBMENUF_SUBMENU ? &submenu : nullptr);
    if (submenu != nullptr && !node.children.empty()) {
      addMenuNodesToTfMenu(submenu, node.children);
    }
  }
}

static bool menuFromJson(ITfMenu *pMenu, const Json::Value &menuInfo) {
  std::vector<ProtocolJson::MenuNode> nodes;
  if (pMenu == nullptr || !ProtocolJson::parseMenuNodes(menuInfo, nodes)) {
    return false;
  }
  addMenuNodesToTfMenu(pMenu, nodes);
  return true;
}

// called when a language bar button needs a menu
// virtual
bool Client::onMenu(LangBarButton *btn, ITfMenu *pMenu) {
  Json::Value result;
  if (sendOnMenu(btn->id(), result)) {
    const Json::Value &menuInfo = result["menuItems"];
    return menuFromJson(pMenu, menuInfo);
  }
  return false;
}

static void appendMenuNodes(HMENU menu,
                            const std::vector<ProtocolJson::MenuNode> &nodes) {
  for (const auto &node : nodes) {
    const std::wstring text = utf8ToUtf16(node.text.c_str());
    UINT flags = MF_STRING;
    UINT id = node.id;
    if (node.separator) {
      flags = MF_SEPARATOR;
    } else {
      if (node.checked)
        flags |= MF_CHECKED;
      if (!node.enabled)
        flags |= MF_GRAYED;
      if (!node.children.empty()) {
        HMENU submenu = ::CreatePopupMenu();
        appendMenuNodes(submenu, node.children);
        flags |= MF_POPUP;
        id = UINT_PTR(submenu);
      }
    }
    AppendMenu(menu, flags, id, text.c_str());
  }
}

static HMENU menuFromJson(const Json::Value &menuInfo) {
  std::vector<ProtocolJson::MenuNode> nodes;
  if (!ProtocolJson::parseMenuNodes(menuInfo, nodes)) {
    return NULL;
  }
  HMENU menu = ::CreatePopupMenu();
  appendMenuNodes(menu, nodes);
  return menu;
}

// called when a language bar button needs a menu
// virtual
HMENU Client::onMenu(LangBarButton *btn) {
  Json::Value result;
  if (sendOnMenu(btn->id(), result)) {
    const Json::Value &menuInfo = result["menuItems"];
    return menuFromJson(menuInfo);
  }
  return NULL;
}

// called when a compartment value is changed
void Client::onCompartmentChanged(const GUID &key) {
  auto guidStr = uuidToString(key);
  if (!guidStr.empty()) {
    auto req = createRpcRequest("onCompartmentChanged");
    req["compartmentGuid"] = guidStr;

    Json::Value ret;
    callRpcMethod(req, ret);
    if (handleRpcResponse(ret, nullptr, false)) {
    }
  }
}

// called when the keyboard is opened or closed
void Client::onKeyboardStatusChanged(bool opened) {
  auto req = createRpcRequest("onKeyboardStatusChanged");
  req["opened"] = opened;

  Json::Value ret;
  callRpcMethod(req, ret);
  if (handleRpcResponse(ret, nullptr, false)) {
  }
}

// called just before current composition is terminated for doing cleanup.
void Client::onCompositionTerminated(bool forced) {
  if (localEngine_.active()) {
    localEngine_.onCompositionTerminated(forced);
  }
  auto req = createRpcRequest("onCompositionTerminated");
  req["forced"] = forced;

  Json::Value ret;
  callRpcMethod(req, ret);
  if (handleRpcResponse(ret, nullptr, false)) {
  }
}

bool Client::init() {
  auto req = createRpcRequest("init");
  req["guid"] = guid_;
  req["isWindows8Above"] = ::IsWindows8OrGreater();
  req["isMetroApp"] = textService_->isMetroApp();
  req["isUiLess"] = textService_->effectiveUiLess();
  req["isConsole"] = textService_->isConsole();

  Json::Value ret;
  return callRpcMethod(req, ret) && handleRpcResponse(ret, nullptr, false);
}

Json::Value Client::createRpcRequest(const char *methodName) {
  Json::Value request(Json::objectValue);
  request["method"] = methodNameToJson(methodName);
  return request;
}

void CALLBACK Client::onAsyncPollTimer(HWND, UINT, UINT_PTR id, DWORD) {
  auto *client = reinterpret_cast<Client *>(id);
  if (client != nullptr) {
    client->pollAsyncResponses();
  }
}

void Client::refreshAsyncPollTimer() {
  // 定时器挂在候选窗上：WM_TIMER 由候选窗过程转发给轮询（见
  // MoqiCandidateWindow::wndProc 的 WM_TIMER 分支）。SetTimer 传窗口句柄时
  // lpTimerFunc 不会被调用，所以必须靠窗口过程。
  HWND targetWindow = nullptr;
  if (textService_ != nullptr && textService_->candidateWindow_ != nullptr &&
      textService_->candidateWindow_->isWindow()) {
    targetWindow = textService_->candidateWindow_->hwnd();
  }

  if (asyncPollTimerId_ != 0 &&
      (targetWindow == nullptr || targetWindow != asyncPollTimerWindow_)) {
    ::KillTimer(asyncPollTimerWindow_, asyncPollTimerId_);
    asyncPollTimerId_ = 0;
    asyncPollTimerWindow_ = nullptr;
  }

  if (targetWindow != nullptr && asyncPollTimerId_ == 0) {
    asyncPollTimerId_ =
        ::SetTimer(targetWindow, reinterpret_cast<UINT_PTR>(this),
                   ASYNC_RPC_POLL_INTERVAL_MS, nullptr);
    asyncPollTimerWindow_ = asyncPollTimerId_ != 0 ? targetWindow : nullptr;
    const DWORD err = asyncPollTimerId_ == 0 ? ::GetLastError() : 0;
    appendRpcGuardLog(L"[pollTimer] set hwnd=0x" +
                      std::to_wstring(reinterpret_cast<UINT_PTR>(targetWindow)) +
                      L" id=" + std::to_wstring(asyncPollTimerId_) +
                      L" gle=" + std::to_wstring(err));
  } else if (targetWindow == nullptr) {
    appendRpcGuardLog(L"[pollTimer] no candidate window; timer not set");
  }
}

bool Client::waitForRpcIdle(int timeoutMs) const {
  const ULONGLONG deadline =
      ::GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
  while (activationInProgress_.load(std::memory_order_acquire) ||
         rpcInProgress_.load(std::memory_order_acquire) > 0) {
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      return false;
    }
    const DWORD sleepMs = static_cast<DWORD>(
        (std::min)(static_cast<ULONGLONG>(RPC_BUSY_POLL_INTERVAL_MS),
                   deadline - now));
    ::Sleep(sleepMs == 0 ? 1 : sleepMs);
  }
  return true;
}

bool Client::readPendingPipeMessage(std::string &serializedReply) {
  serializedReply.clear();
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD bytesAvailable = 0;
  if (!::PeekNamedPipe(pipe_, nullptr, 0, nullptr, &bytesAvailable, nullptr) ||
      bytesAvailable == 0) {
    return false;
  }
  appendRpcTraceLog(L"[poll] pipe has " + std::to_wstring(bytesAvailable) +
                    L" bytes pending");

  // Data is already available, so the overlapped read completes immediately;
  // the short timeout only guards against a race with the server closing.
  return readPipeMessageWithTimeout(pipe_, serializedReply, 250, nullptr);
}

void Client::enqueueAsyncResponse(const Json::Value &msg) {
  pendingAsyncResponses_.push_back(msg);
}

bool Client::applyAsyncResponse(Json::Value &msg, Ime::EditSession *session) {
  // 云剪贴板推送帧带 cloudClip 标记：进入/退出转发模式（所有键走 RPC，
  // 本地引擎不处理——否则 rime 吃掉选字键刷掉列表）。
  if (msg.isMember("cloudClip")) {
    cloudClipRpcMode_ = msg["cloudClip"].asBool();
    appendRpcGuardLog(L"[cloudclip] rpc mode " +
                      std::wstring(cloudClipRpcMode_ ? L"ON" : L"OFF"));
  }
  if (localEngine_.active() && msg.get("aiOverlay", false).asBool()) {
    // 进程内引擎：AI 结果只叠到本地覆盖层（显示/选字由 LocalEngine 管），
    // 帧里的引擎状态是 daemon 侧的空会话，不能直接套用。compositionString
    // 是生成时的组字快照，与当前组字不一致即过期丢弃（用户已上屏/改动）。
    Json::Value state;
    localEngine_.getCompositionState(state);
    const std::string prompt = msg.get("compositionString", "").asString();
    const Json::Value &entries = msg["candidateEntries"];
    const std::string text = entries.isArray() && entries.size() > 0
                                 ? entries[0].get("text", "").asString()
                                 : "";
    if (text.empty() || state["composition"].asString() != prompt) {
      appendRpcTraceLog(L"[ai] overlay dropped (stale or empty) prompt=" +
                        utf8ToUtf16(prompt.c_str()));
      return true;
    }
    localEngine_.setAiOverlay(text);
    Json::Value resp;
    if (!localEngine_.redraw(resp)) {
      return true;
    }
    // 答案可能带换行（写好评三连），压成单行再入日志。
    std::string oneLine = text;
    for (char &c : oneLine) {
      if (c == '\n' || c == '\r') {
        c = ' ';
      }
    }
    appendRpcTraceLog(L"[ai] overlay applied text=" +
                      utf8ToUtf16(oneLine.c_str()) +
                      L" session=" + (session != nullptr ? L"1" : L"0"));
    return handleRpcResponse(resp, session, true);
  }
  return handleRpcResponse(msg, session, true);
}

void Client::flushPendingAsyncResponses(Ime::EditSession *session) {
  while (!pendingAsyncResponses_.empty()) {
    Json::Value msg = pendingAsyncResponses_.front();
    if (!applyAsyncResponse(msg, session)) {
      break;
    }
    pendingAsyncResponses_.pop_front();
  }
}

void Client::flushPendingAsyncResponsesWithCurrentContext() {
  if (pendingAsyncResponses_.empty() || asyncFlushInProgress_) {
    return;
  }
  if (textService_ == nullptr) {
    flushPendingAsyncResponses();
    return;
  }

  auto context = textService_->currentContext();
  if (!context) {
    appendRpcGuardLog(L"async response pending but current context is unavailable");
    return;
  }

  HRESULT sessionResult = E_FAIL;
  asyncFlushInProgress_ = true;
  auto editSession = Ime::ComPtr<Ime::EditSession>::make(
      context,
      [this](Ime::EditSession *session, TfEditCookie) {
        flushPendingAsyncResponses(session);
        asyncFlushInProgress_ = false;
      });
  context->RequestEditSession(textService_->clientId(), editSession,
                              TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                              &sessionResult);
  if (FAILED(sessionResult)) {
    asyncFlushInProgress_ = false;
    appendRpcGuardLog(L"async response RequestEditSession failed hr=" +
                      std::to_wstring(static_cast<long>(sessionResult)));
  }
}

void Client::pollAsyncResponses() {
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return;
  }

  std::string serializedResponse;
  while (readPendingPipeMessage(serializedResponse)) {
    Proto::FrameBuffer responseBuffer;
    responseBuffer.append(serializedResponse.data(), serializedResponse.size());
    std::string payload;
    while (responseBuffer.nextFrame(payload)) {
      Json::Value msg;
      if (!Proto::parseJsonPayload(payload, msg)) {
        closeRpcConnection();
        return;
      }
      appendRpcTraceLog(L"[poll] dequeued frame seq=" +
                        std::to_wstring(msg.get("seqNum", -1).asInt()) +
                        L" aiOverlay=" +
                        std::to_wstring(
                            msg.get("aiOverlay", false).asBool() ? 1 : 0));
      enqueueAsyncResponse(msg);
    }
  }

  flushPendingAsyncResponsesWithCurrentContext();
}

namespace {

// Waits for an overlapped operation that is already in flight on |pipe|.
// Returns ERROR_SUCCESS when it completed, ERROR_MORE_DATA when a message-mode
// read filled the buffer, ERROR_TIMEOUT after cancelling it, or a Win32 error.
DWORD waitOverlappedResult(HANDLE pipe, OVERLAPPED &overlapped,
                           DWORD timeoutMs, DWORD &transferred) {
  if (::WaitForSingleObject(overlapped.hEvent, timeoutMs) != WAIT_OBJECT_0) {
    ::CancelIoEx(pipe, &overlapped);
    ::WaitForSingleObject(overlapped.hEvent, INFINITE);
    return ERROR_TIMEOUT;
  }
  if (::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
    return ERROR_SUCCESS;
  }
  return ::GetLastError();
}

} // namespace

// One overlapped ReadFile of up to |want| bytes, appended to |out|. In BYTE
// read mode the pipe delivers whatever is available (>= 1 byte or error).
// Returns ERROR_SUCCESS, ERROR_TIMEOUT, or a Win32 error. A zero-byte
// successful read means EOF and is reported as ERROR_BROKEN_PIPE.
static DWORD readPipeChunk(HANDLE pipe, size_t want, std::string &out,
                           ULONGLONG deadline) {
  char buf[4096];
  OVERLAPPED overlapped = {};
  overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) {
    return ERROR_NOT_ENOUGH_MEMORY;
  }
  const DWORD chunk = static_cast<DWORD>((std::min)(want, sizeof(buf)));
  DWORD rlen = 0;
  DWORD result = ERROR_SUCCESS;
  if (!::ReadFile(pipe, buf, chunk, &rlen, &overlapped)) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_IO_PENDING) {
      const ULONGLONG now = ::GetTickCount64();
      const DWORD remaining =
          now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
      result = waitOverlappedResult(pipe, overlapped, remaining, rlen);
    } else {
      result = error;
    }
  }
  ::CloseHandle(overlapped.hEvent);
  if (result == ERROR_SUCCESS) {
    if (rlen == 0) {
      return ERROR_BROKEN_PIPE;
    }
    out.append(buf, rlen);
  }
  return result;
}

// Reads exactly one framed message (4-byte little-endian length prefix +
// payload) and returns it INCLUDING the prefix, matching what a message-mode
// read used to deliver. The pipe is kept in BYTE read mode: overlapped
// message-mode reads silently dropped the middle chunks of frames larger
// than the 4096 buffer (the 15KB onMenu reply assembled to 6.9KB and the
// JSON parse failed on every menu request; 2026-11-02, probe3 repro).
// A timeout before the first byte is a "nothing arrived yet" timeout and is
// reported through |timedOut| (safe to keep the connection); a timeout with
// partial data desyncs the byte stream, so it is reported as a hard error
// and the caller closes the connection.
bool Client::readPipeMessageWithTimeout(HANDLE pipe, std::string &message,
                                        DWORD timeoutMs, bool *timedOut) {
  message.clear();
  if (timedOut != nullptr) {
    *timedOut = false;
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;

  while (message.size() < 4) {
    const DWORD result =
        readPipeChunk(pipe, 4 - message.size(), message, deadline);
    if (result == ERROR_TIMEOUT) {
      if (timedOut != nullptr && message.empty()) {
        *timedOut = true;
      }
      return false;
    }
    if (result != ERROR_SUCCESS) {
      return false;
    }
  }
  std::uint32_t payloadSize = 0;
  std::memcpy(&payloadSize, message.data(), sizeof(payloadSize));
  // Guard against a corrupted stream: the backend never sends frames this big.
  if (payloadSize > 64U * 1024 * 1024) {
    return false;
  }
  const size_t total = 4 + static_cast<size_t>(payloadSize);
  while (message.size() < total) {
    const DWORD result =
        readPipeChunk(pipe, total - message.size(), message, deadline);
    if (result != ERROR_SUCCESS) {
      // Mid-frame timeouts desync the byte stream: report a hard error so
      // the caller tears the connection down instead of keeping it warm.
      return false;
    }
  }
  return true;
}

// Writes one request as a single pipe message. The pipe is overlapped, so the
// write normally completes immediately; the timeout only bounds a pathological
// "backend stuck with a full pipe" case.
bool Client::writePipeMessageWithTimeout(HANDLE pipe, const char *data,
                                         size_t len, DWORD timeoutMs) {
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  OVERLAPPED overlapped = {};
  overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) {
    return false;
  }

  DWORD written = 0;
  DWORD result = ERROR_SUCCESS;
  if (!::WriteFile(pipe, data, static_cast<DWORD>(len), &written,
                   &overlapped)) {
    const DWORD error = ::GetLastError();
    result = error == ERROR_IO_PENDING
                 ? waitOverlappedResult(pipe, overlapped, timeoutMs, written)
                 : error;
  }
  ::CloseHandle(overlapped.hEvent);
  return result == ERROR_SUCCESS && written == len;
}

bool Client::callRpcPipe(HANDLE pipe, const std::string &serializedRequest,
                         std::string &serializedReply, bool *timedOut) {
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  // Write the request as one pipe message, then wait for the reply with a
  // bounded deadline. Both operations are overlapped and event-driven.
  if (!writePipeMessageWithTimeout(pipe, serializedRequest.data(),
                                   serializedRequest.size(),
                                   RPC_WRITE_TIMEOUT_MS)) {
    return false;
  }
  return readPipeMessageWithTimeout(pipe, serializedReply,
                                    RPC_REPLY_TIMEOUT_MS, timedOut);
}

// send the request to the server
// a sequence number will be added to the req object automatically.
bool Client::callRpcMethod(Json::Value &request, Json::Value &response) {
  try {
    ScopedRpcInProgress rpcGuard(rpcInProgress_);
    if (shouldWaitConnection_ && !waitForRpcConnection(connectAttempts_)) {
      appendRpcGuardLog(L"[callRpcMethod] connect failed, method=" +
                        utf8ToUtf16(request.get("method", "").asCString()));
      return false;
    }

    pollAsyncResponses();

    // 单次 请求→读响应 往返；失败原因经 timedOut 带出。
    bool timedOut = false;
    auto attemptRpc = [&]() -> bool {
      const auto seqNum = nextSeqNum_++;
      request["seqNum"] = seqNum;
      std::string serializedRequest;
      if (!Proto::serializeJsonMessage(request, serializedRequest)) {
        return false;
      }
      std::string serializedResponse;
      timedOut = false;
      if (!callRpcPipe(pipe_, serializedRequest, serializedResponse,
                       &timedOut)) {
        return false;
      }
      while (true) {
        Proto::FrameBuffer responseBuffer;
        responseBuffer.append(serializedResponse.data(),
                              serializedResponse.size());
        std::string payload;
        Json::Value msg;
        if (!responseBuffer.nextFrame(payload) ||
            !Proto::parseJsonPayload(payload, msg)) {
          return false;
        }
        if (msg.get("seqNum", -1).asInt() == seqNum) {
          response = msg;
          return true;
        }
        enqueueAsyncResponse(msg);
        serializedResponse.clear();
        // The synchronous reply may arrive in a later pipe message than an
        // async notification; wait for it with a bounded timeout.
        if (!readPipeMessageWithTimeout(pipe_, serializedResponse,
                                        RPC_ASYNC_REPLY_WAIT_MS, &timedOut)) {
          return false;
        }
      }
    };

    bool success = attemptRpc();

    // 2026-11-01 管道自愈：本地引擎接管后按键不走管道，pipe 在长空闲期变陈旧。
    // 原重试条件要求 pipe_==INVALID，但 callRpcPipe 按值收句柄、失败时不改
    // pipe_，陈旧句柄看起来仍然有效 → 重试永不触发，菜单/命令类 RPC 全灭。
    // 现在改为：只要是真错（不是已握手连接上的纯超时），就强制关闭管道、
    // 重连并重试一次。纯超时仍保留"连接保温"策略（冷启动后端慢，不是死）。
    if (!success && (!timedOut || !handshakeComplete_)) {
      appendRpcGuardLog(
          L"[callRpcMethod] pipe error, reconnect+retry method=" +
          utf8ToUtf16(request.get("method", "").asCString()) +
          L" timedOut=" + std::to_wstring(timedOut ? 1 : 0) + L" pipe=" +
          std::to_wstring(pipe_ != INVALID_HANDLE_VALUE ? 1 : 0));
      closeRpcConnection();
      if (!shouldWaitConnection_ || waitForRpcConnection(connectAttempts_)) {
        success = attemptRpc();
      }
      if (!success) {
        appendRpcGuardLog(L"[callRpcMethod] reconnect+retry failed");
      }
    }

    flushPendingAsyncResponses();

    if (!success) {
      // A pure timeout on an already-initialized connection usually means the
      // backend is slow (cold start: first request per engine can take seconds),
      // not dead. Keep the connection warm so the next request does not pay
      // for a reconnect + re-init cycle, which made cold starts even slower
      // (each reconnect created a fresh engine that had to reload everything).
      // Genuine pipe errors and pre-handshake timeouts still close the
      // connection so it can be re-established and re-initialized.
      if (!timedOut || !handshakeComplete_) {
        closeRpcConnection();    // close the pipe connection since it's broken
        resetTextServiceState(); // since we lost the connection, the state is
                                 // unknown so we reset.
      }
    }
    return success;
  } catch (const std::exception &) {
    appendRpcGuardLog(L"[callRpcMethod] caught std::exception; closing RPC connection");
  } catch (...) {
    appendRpcGuardLog(L"[callRpcMethod] caught unknown exception; closing RPC connection");
  }
  closeRpcConnection();
  resetTextServiceState();
  return false;
}

bool Client::isPipeCreatedByMoqiServer(HANDLE pipe) {
  // security check: make sure that we're connecting to the correct server
  ULONG serverPid;
  if (GetNamedPipeServerProcessId(pipe, &serverPid)) {
    // FIXME: check the command line of the server?
    // See this:
    // http://www.codeproject.com/Articles/19685/Get-Process-Info-with-NtQueryInformationProcess
    // Too bad! Undocumented Windows internal API might be needed here. :-(
  }
  return true;
}

// establish a connection to the specified pipe and returns its handle
// static
HANDLE Client::connectPipe(const wchar_t *pipeName, int timeoutMs) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  if (WaitNamedPipe(pipeName, timeoutMs)) {
    // FILE_FLAG_OVERLAPPED lets readPipeMessageWithTimeout wait on an event
    // instead of polling PeekNamedPipe + Sleep(2ms) on every RPC.
    pipe = CreateFile(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
      appendRpcGuardLog(L"[connectPipe] CreateFile failed err=" +
                        std::to_wstring(::GetLastError()));
    }
  } else {
    appendRpcGuardLog(L"[connectPipe] WaitNamedPipe failed timeout=" +
                      std::to_wstring(timeoutMs) + L" err=" +
                      std::to_wstring(::GetLastError()));
  }

  if (pipe != INVALID_HANDLE_VALUE) {
    // 2026-11-02: stay in BYTE read mode. Message mode made multi-chunk
    // overlapped reads drop data for frames > 4096 bytes (see
    // readPipeMessageWithTimeout); framing is explicit via the length prefix.
    if (!isPipeCreatedByMoqiServer(pipe)) {
      appendRpcGuardLog(L"[connectPipe] security check failed err=" +
                        std::to_wstring(::GetLastError()));
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
    }
  }
  return pipe;
}

// True when this text service is hosted in a SYSTEM-context process (e.g. the
// logon screen / LogonUI). In that context we must NOT start MoqiLauncher:
// a launcher started by SYSTEM binds its pipe with a DACL that user processes
// cannot open and spawns the backend under the SYSTEM profile. The user-session
// autostart (Run key + scheduled task) is the only correct owner of the
// launcher; the client just waits for it.
static bool isSystemAccount() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  bool isSystem = false;
  DWORD size = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> buffer(size, 0);
  TOKEN_USER *user = reinterpret_cast<TOKEN_USER *>(buffer.data());
  if (::GetTokenInformation(token, TokenUser, user, size, &size) &&
      user->User.Sid != nullptr) {
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID systemSid = nullptr;
    if (::AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                   0, 0, 0, 0, 0, 0, 0, &systemSid)) {
      isSystem = ::EqualSid(user->User.Sid, systemSid);
      ::FreeSid(systemSid);
    }
  }
  ::CloseHandle(token);
  return isSystem;
}

// Send a notification to the server without waiting for the reply.
// Used for fire-and-forget notifications (deactivate, composition terminated,
// userdbChanged 学习合并通知) so a slow/hung backend can never block the
// TSF/background thread.
void Client::sendRpcNoWait(const char *methodName) {
  if (pipe_ == INVALID_HANDLE_VALUE) {
    return;
  }
  auto req = createRpcRequest(methodName);
  std::string serializedRequest;
  if (!Proto::serializeJsonMessage(req, serializedRequest)) {
    return;
  }
  // Fire-and-forget: a short bounded overlapped write so a stuck backend can
  // never block the TSF thread.
  writePipeMessageWithTimeout(pipe_, serializedRequest.data(),
                              serializedRequest.size(), 250);
}

// 通知的可靠版（只准在后台线程调，重连可能阻塞 ~1s）：本地引擎进程平时
// 不走管道，句柄可能已陈旧——用户词典合并通知曾因陈旧句柄静默丢失
//（2026-11-02 实证），这里允许一次轻量重连。
void Client::sendRpcNoWaitReliable(const char *methodName) {
  if (pipe_ == INVALID_HANDLE_VALUE && !waitForRpcConnection(2)) {
    appendRpcGuardLog(L"[notify] " + utf8ToUtf16(methodName) +
                      L" failed: no connection");
    return;
  }
  auto req = createRpcRequest(methodName);
  std::string serializedRequest;
  if (!Proto::serializeJsonMessage(req, serializedRequest)) {
    return;
  }
  if (writePipeMessageWithTimeout(pipe_, serializedRequest.data(),
                                  serializedRequest.size(), 250)) {
    appendRpcTraceLog(L"[notify] " + utf8ToUtf16(methodName) + L" sent");
    return;
  }
  appendRpcGuardLog(L"[notify] " + utf8ToUtf16(methodName) +
                    L" write failed; reconnecting");
  closeRpcConnection();
  if (!waitForRpcConnection(2)) {
    appendRpcGuardLog(L"[notify] " + utf8ToUtf16(methodName) +
                      L" reconnect failed");
    return;
  }
  const bool ok = writePipeMessageWithTimeout(
      pipe_, serializedRequest.data(), serializedRequest.size(), 250);
  appendRpcTraceLog(L"[notify] " + utf8ToUtf16(methodName) + L" retry ok=" +
                    std::to_wstring(ok ? 1 : 0));
}

bool Client::ensureLauncherRunning() {
  // Never start the launcher from a SYSTEM-context process (logon screen):
  // it would create a launcher/backend that user apps cannot use.
  if (isSystemAccount()) {
    return false;
  }
  // Retryable: unlike the previous one-shot attempt, we allow re-launching
  // the launcher with a cooldown so a failed early attempt (e.g. during the
  // logon screen before the shell is ready) no longer poisons the client
  // forever.
  const ULONGLONG now = ::GetTickCount64();
  if (now - lastLauncherStartAttemptTick_ < LAUNCHER_START_RETRY_COOLDOWN_MS) {
    appendRpcGuardLog(L"[ensureLauncher] cooldown, skip launch");
    return false;
  }
  lastLauncherStartAttemptTick_ = now;

  auto module =
      static_cast<Moqi::ImeModule *>(textService_->imeModule().operator->());
  if (module == nullptr || module->programDir().empty()) {
    return false;
  }

  // 2026-11-02 阶段5c：MoqiDaemon.exe --serve 自己就是管道服务端
  //（MoqiLaunch 的职责已并入）。优先拉它；老安装没有 daemon 时回退
  // MoqiLaunch.exe（退休期兼容，下个版本移除回退）。
  std::wstring execPath = module->programDir();
  execPath += L"\\MoqiDaemon.exe";
  std::wstring commandLine = L"\"" + execPath + L"\" --serve";
  DWORD attrs = ::GetFileAttributesW(execPath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    execPath = module->programDir();
    execPath += L"\\MoqiLaunch.exe";
    commandLine = L"\"" + execPath + L"\"";
    attrs = ::GetFileAttributesW(execPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      return false;
    }
    appendRpcGuardLog(L"[ensureLauncher] fallback to MoqiLaunch.exe");
  } else {
    appendRpcGuardLog(L"[ensureLauncher] launching MoqiDaemon.exe --serve");
  }

  // Use CreateProcessW instead of ShellExecuteW: it cannot fail on the secure
  // desktop / early logon and we control the working directory explicitly.
  STARTUPINFOW startupInfo = {};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo = {};
  const BOOL created = ::CreateProcessW(
      nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
      nullptr, module->programDir().c_str(), &startupInfo, &processInfo);
  if (created) {
    ::CloseHandle(processInfo.hThread);
    ::CloseHandle(processInfo.hProcess);
    return true;
  }
  appendRpcGuardLog(L"[ensureLauncher] CreateProcess failed err=" +
                    std::to_wstring(::GetLastError()));
  return false;
}

// Check whether a MoqiLauncher process exists (via its single-instance mutex).
bool Client::isLauncherProcessRunning() {
  HANDLE mutex = ::OpenMutexW(MUTEX_ALL_ACCESS, FALSE, L"MoqiLaunchMutex");
  if (mutex != nullptr) {
    ::CloseHandle(mutex);
    return true;
  }
  return false;
}

// Last-resort self-heal: a MoqiLauncher process exists but its pipe is not
// reachable, which means it is stuck (dead uv loop / unbound pipe). Force-kill
// it and start a fresh one — the same thing the user does manually
// ("kill MoqiLauncher.exe, then switch the input method").
bool Client::forceRestartLauncher() {
  const ULONGLONG now = ::GetTickCount64();
  if (now - lastLauncherKillTick_ < LAUNCHER_KILL_RETRY_COOLDOWN_MS) {
    appendRpcGuardLog(L"[launcher] force-restart cooldown, skip");
    return false;
  }
  lastLauncherKillTick_ = now;
  appendRpcGuardLog(L"[launcher] force-restarting unreachable backend host");

  wchar_t systemDir[MAX_PATH] = {};
  if (::GetSystemDirectoryW(systemDir, _countof(systemDir)) == 0) {
    return false;
  }
  std::wstring taskkillPath = std::wstring(systemDir) + L"\\taskkill.exe";
  // 阶段5c：daemon（serve 模式）与退休期的 MoqiLaunch 都要清掉，
  // 否则残留进程继续占着管道。
  const wchar_t *images[] = {L"MoqiDaemon.exe", L"MoqiLaunch.exe"};
  for (const wchar_t *image : images) {
    std::wstring commandLine =
        L"\"" + taskkillPath + L"\" /F /IM " + image;
    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};
    if (::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo,
                         &processInfo)) {
      ::CloseHandle(processInfo.hThread);
      ::CloseHandle(processInfo.hProcess);
    }
  }
  // Give the old process (and its children) a moment to die before relaunch.
  ::Sleep(300);
  return ensureLauncherRunning();
}

// Ensure that we're connected to the Moqi input method server
// If we are already connected, the method simply returns true;
// otherwise, it tries to establish the connection with a bounded budget.
bool Client::waitForRpcConnection(int connectAttempts) {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    return true;
  }

  appendRpcTraceLog(L"[connect] begin attempts=" +
                    std::to_wstring(connectAttempts));
  wstring serverPipeName = getPipeName(L"Launcher");
  pipe_ = connectPipe(serverPipeName.c_str(), 0);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    ensureLauncherRunning();
  }

  for (int attempt = 0; pipe_ == INVALID_HANDLE_VALUE && attempt < connectAttempts;
       ++attempt) {
    // try to connect to the server
    pipe_ = connectPipe(serverPipeName.c_str(), CONNECT_ATTEMPT_TIMEOUT_MS);
  }

  if (pipe_ == INVALID_HANDLE_VALUE && isLauncherProcessRunning()) {
    // A launcher process exists but we cannot reach its pipe: it is stuck.
    // Force a restart and retry once.
    appendRpcGuardLog(L"[connect] launcher alive but pipe unreachable; "
                      L"force restart");
    forceRestartLauncher();
    for (int attempt = 0;
         pipe_ == INVALID_HANDLE_VALUE && attempt < connectAttempts; ++attempt) {
      pipe_ = connectPipe(serverPipeName.c_str(), CONNECT_ATTEMPT_TIMEOUT_MS);
    }
  }

  if (pipe_ != INVALID_HANDLE_VALUE) {
    const bool handshakeOk = completeConnectionHandshake();
    appendRpcTraceLog(L"[connect] handshake ok=" +
                      std::to_wstring(handshakeOk ? 1 : 0));
    return handshakeOk;
  }
  appendRpcGuardLog(L"[connect] give up (no pipe, launcherRunning=" +
                    std::to_wstring(isLauncherProcessRunning() ? 1 : 0) + L")");
  return false;
}

bool Client::completeConnectionHandshake() {
  // send initialization info to the server for hand-shake.
  shouldWaitConnection_ =
      false; // prevent recursive call of waitForRpcConnection
  const bool initOk = init();
  if (!initOk) {
    closeRpcConnection();
    shouldWaitConnection_ = true;
    return false;
  }
  handshakeComplete_ = true;

  if (isActivated_) {
    // we lost connection while being activated previously
    // re-initialize the whole text service.
    // activate the text service again.
    onActivate();
  }
  shouldWaitConnection_ = true;
  // if init() or onActivate() RPC fails, the pipe_ might have been closed.
  return pipe_ != INVALID_HANDLE_VALUE;
}

void Client::resetTextServiceState() {
  // we lost connection while being activated previously
  // re-initialize the whole text service.

  // cleanup for the previous instance.
  // remove all buttons

  // some language bar buttons are not unregistered properly
  if (!buttons_.empty()) {
    appendRpcGuardLog(L"[reset] removing buttons n=" +
                      std::to_wstring(buttons_.size()));
    for (auto &item : buttons_) {
      textService_->removeButton(item.second);
    }
    buttons_.clear();
  }
  autoPairRules_ = defaultAutoPairRules();
}

void Client::closeRpcConnection() {
  pendingAsyncResponses_.clear();
  handshakeComplete_ = false;
  if (asyncPollTimerId_ != 0) {
    ::KillTimer(asyncPollTimerWindow_, asyncPollTimerId_);
    asyncPollTimerId_ = 0;
    asyncPollTimerWindow_ = nullptr;
  }
  if (pipe_ != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}

wstring Client::getPipeName(const wchar_t *base_name) {
  wstring pipeName = L"\\\\.\\pipe\\";
  DWORD len = 0;
  ::GetUserNameW(NULL, &len); // get the required size of the buffer
  if (len <= 0)
    return wstring();
  // add username to the pipe path so it won't clash with the other users' pipes
  unique_ptr<wchar_t[]> username(new wchar_t[len]);
  if (!::GetUserNameW(username.get(), &len))
    return wstring();
  pipeName += username.get();
  pipeName += L"\\MoqiIM\\";
  pipeName += base_name;
  return pipeName;
}

} // namespace Moqi
