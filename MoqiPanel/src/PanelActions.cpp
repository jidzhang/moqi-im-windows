// 按键闸门、引擎回包、复制与翻译流程。
//
// 按键闸门（本文件最要紧的一处设计）：
//   消息循环对输入区的键盘消息**不调用** TranslateMessage，先扣住送引擎；
//   引擎回"吃了"就丢弃，回"没吃"才用扣住那一刻的键盘状态重新翻译并回放给
//   编辑框。这样 (1) 键永不丢——引擎再慢也只是晚到；(2) 回放出的字符与
//   当时真实按键一致，不受 Shift 是否已松开影响；(3) 与 TSF 的两阶段语义
//   （OnTestKeyDown 判吃没吃，吃了才 OnKeyDown）逐键等价，所以中文/英文、
//   标点、Ctrl 组合键的行为与装着墨奇输入法的普通应用里一模一样。
#include <windows.h>

#include <commctrl.h>

#include <cstring>
#include <memory>
#include <thread>

#include "PanelCore.h"
#include "PanelMessages.h"
#include "PanelStyle.h"
#include "PanelWindow.h"
#include "SettingsDialog.h"

namespace MoqiPanel {
namespace {

// 与 libIME2 同口径：清掉 Ctrl/Alt 后 ToUnicode，组合键也能得到底下的可打印
// 字符（引擎靠它区分大小写与标点键）。
UINT charCodeFor(UINT vk, UINT scanCode, const BYTE states[256]) {
  BYTE translated[256];
  std::memcpy(translated, states, sizeof(translated));
  translated[VK_CONTROL] = 0;
  translated[VK_MENU] = 0;
  WCHAR buffer[8] = {};
  const int count =
      ::ToUnicodeEx(vk, scanCode, translated, buffer, 8, 0, ::GetKeyboardLayout(0));
  return count == 1 ? static_cast<UINT>(buffer[0]) : 0;
}

} // namespace

bool PanelWindow::holdKeyMessage(const MSG& msg) const {
  return msg.hwnd == edit_ && isKeyMessage(msg.message);
}

// ---- 按键闸门 -------------------------------------------------------------

LRESULT CALLBACK PanelWindow::editSubclassProc(HWND edit, UINT msg, WPARAM wp,
                                               LPARAM lp, UINT_PTR, DWORD_PTR refData) {
  auto* self = reinterpret_cast<PanelWindow*>(refData);
  switch (msg) {
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(edit, &PanelWindow::editSubclassProc, 1);
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (edit == self->edit_) return self->onEditKeyDown(edit, msg, wp, lp);
      break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (edit == self->edit_) return self->onEditKeyUp(edit, msg, wp, lp);
      break;
    case WM_COPY:
      self->copySelection(edit);
      return 0;
    case WM_CUT:
      if (edit == self->edit_ && self->copySelection(edit)) {
        ::SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
      }
      return 0;
    case WM_LBUTTONUP: {
      // 鼠标选中（拖选、双击选词、Shift+单击扩选）在松开时定局：选中即复制。
      const LRESULT result = ::DefSubclassProc(edit, msg, wp, lp);
      self->copySelection(edit);
      return result;
    }
    case WM_SETFOCUS:
      // 结果区不该留住键盘焦点：点进去选中文字后焦点还给输入区，接着打字不丢。
      if (edit != self->edit_) {
        const LRESULT result = ::DefSubclassProc(edit, msg, wp, lp);
        ::PostMessageW(self->hwnd(), kMsgRefocusEdit, 0, 0);
        return result;
      }
      break;
    default:
      break;
  }
  return ::DefSubclassProc(edit, msg, wp, lp);
}

LRESULT PanelWindow::onEditKeyDown(HWND edit, UINT message, WPARAM wp, LPARAM lp) {
  const UINT vk = static_cast<UINT>(wp);
  BYTE states[256] = {};
  ::GetKeyboardState(states);
  const bool ctrl = (states[VK_CONTROL] & 0x80) != 0;
  const bool shift = (states[VK_SHIFT] & 0x80) != 0;
  const bool alt = (lp & 0x20000000) != 0;  // lParam 位 29：按下时 Alt 是否按着
  // 面板自己的快捷键：不进引擎，同步生效（它们的键松由 onEditKeyUp 吞掉）。
  if (ctrl && !alt && !shift && vk == VK_RETURN) {
    swallowedUps_.insert(vk);
    doTranslate();
    return 0;
  }
  if (ctrl && !alt && !shift && vk == 'A') {
    swallowedUps_.insert(vk);
    ::SendMessageW(edit, EM_SETSEL, 0, -1);
    copySelection(edit);
    return 0;
  }
  holdKey(edit, message, wp, lp, states);
  return 0;
}

LRESULT PanelWindow::onEditKeyUp(HWND edit, UINT message, WPARAM wp, LPARAM lp) {
  const UINT vk = static_cast<UINT>(wp);
  if (swallowedUps_.erase(vk) != 0) return 0;
  BYTE states[256] = {};
  ::GetKeyboardState(states);
  // 键盘拉选区（Shift+方向键等）：这一下按下要等引擎回包后才真正回放，
  // 所以延后一点再看选区。
  if (isNavigationKey(vk) && (states[VK_SHIFT] & 0x80) != 0) {
    ::SetTimer(hwnd_, kTimerSelection, 220, nullptr);
  }
  holdKey(edit, message, wp, lp, states);
  return 0;
}

void PanelWindow::holdKey(HWND edit, UINT message, WPARAM wp, LPARAM lp,
                          const BYTE states[256]) {
  HeldKey held;
  held.id = nextKeyId_++;
  held.msg.hwnd = edit;
  held.msg.message = message;
  held.msg.wParam = wp;
  held.msg.lParam = lp;
  std::memcpy(held.states, states, sizeof(held.states));
  if (engineStatus_ == EngineStatus::Failed) {
    // 引擎不可用：不扣键，原样放行（字母以英文上屏，可见、可删）。
    replayKey(held);
    return;
  }
  held_.push_back(held);
  KeyInput key;
  key.vk = static_cast<UINT>(wp);
  key.scanCode = static_cast<UINT>((lp >> 16) & 0xFF);
  key.isExtended = (lp & 0x01000000) != 0;
  key.isUp = message == WM_KEYUP || message == WM_SYSKEYUP;
  std::memcpy(key.states, states, sizeof(key.states));
  key.charCode = charCodeFor(key.vk, key.scanCode, states);
  engine_->postKey(key, held.id);
}

void PanelWindow::replayKey(const HeldKey& held) {
  // 用扣住那一刻的键盘状态重新翻译：不能依赖回放时的真实键盘（Shift 可能
  // 已经松开）。TranslateMessage 产出的 WM_CHAR 排进队列，由消息循环照常分发。
  BYTE saved[256];
  std::memcpy(saved, held.states, sizeof(saved));
  BYTE now[256];
  const bool haveNow = ::GetKeyboardState(now) != FALSE;
  ::SetKeyboardState(saved);
  MSG msg = held.msg;
  ::TranslateMessage(&msg);
  if (haveNow) ::SetKeyboardState(now);
  ::DefSubclassProc(held.msg.hwnd, held.msg.message, held.msg.wParam, held.msg.lParam);
}

void PanelWindow::resolveHeld(uint64_t id, bool ok, bool eaten) {
  for (HeldKey& held : held_) {
    if (held.id != id) continue;
    held.resolved = true;
    held.eaten = ok && eaten;
    break;
  }
  drainHeld();
}

void PanelWindow::drainHeld() {
  // 必须按扣住的先后顺序回放：前一个键还没定，后一个键再明确也得等。
  while (!held_.empty() && held_.front().resolved) {
    HeldKey held = std::move(held_.front());
    held_.pop_front();
    if (!held.eaten) replayKey(held);
  }
}

// ---- 复制（四条路径都过 convertPunctuation 这一个出口）--------------------

bool PanelWindow::copyText(HWND owner, const std::wstring& text) {
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) return false;
  void* data = ::GlobalLock(memory);
  if (data == nullptr) {
    ::GlobalFree(memory);
    return false;
  }
  std::memcpy(data, text.c_str(), bytes);
  ::GlobalUnlock(memory);
  // 剪贴板是全局资源，别的程序可能正开着：短重试几次。
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (::OpenClipboard(owner)) {
      ::EmptyClipboard();
      const bool ok = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
      ::CloseClipboard();
      if (ok) return true;  // 成功后内存归系统
    }
    ::Sleep(25);
  }
  ::GlobalFree(memory);
  return false;
}

bool PanelWindow::copySelection(HWND edit) {
  DWORD start = 0;
  DWORD end = 0;
  ::SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));
  if (start == end) return false;
  const std::wstring text = controlText(edit);
  if (end > text.size()) return false;
  const std::wstring selection = text.substr(start, end - start);
  if (trimmed(selection).empty()) return false;
  if (copyText(hwnd_, convertPunctuation(selection))) {
    setStatus(L"已复制选中内容（" + std::to_wstring(codePointCount(selection)) +
                  L" 字，中文标点已换成英文标点）。",
              StatusKind::Success);
    return true;
  }
  setStatus(L"复制失败：剪贴板正被别的程序占用，请重试。", StatusKind::Error);
  return false;
}

void PanelWindow::autoCopySelections() { copySelection(edit_); }

void PanelWindow::doCopySource() {
  const std::wstring text = trimmed(controlText(edit_));
  if (text.empty()) {
    setStatus(L"输入区是空的，没有可复制的文字。", StatusKind::Warn);
    return;
  }
  if (copyText(hwnd_, convertPunctuation(text))) {
    setStatus(L"已复制原文（" + std::to_wstring(codePointCount(text)) +
                  L" 字，中文标点已换成英文标点）。",
              StatusKind::Success);
  } else {
    setStatus(L"复制失败：剪贴板正被别的程序占用，请重试。", StatusKind::Error);
  }
}

void PanelWindow::doRecopy() {
  if (lastTranslation_.empty()) return;
  if (copyText(hwnd_, convertPunctuation(lastTranslation_))) {
    setStatus(L"已重新复制译文（" + std::to_wstring(codePointCount(lastTranslation_)) +
                  L" 字，中文标点已换成英文标点）。",
              StatusKind::Success);
  } else {
    setStatus(L"复制失败：剪贴板正被别的程序占用，请重试。", StatusKind::Error);
  }
}

void PanelWindow::doRestore() {
  if (lastSource_.empty()) return;
  // 放回输入区光标处，不覆盖已有内容（先把选区收起到末尾）；一次性。
  DWORD start = 0;
  DWORD end = 0;
  ::SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));
  ::SendMessageW(edit_, EM_SETSEL, end, end);
  ::SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(lastSource_.c_str()));
  ::SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  setStatus(L"已把上一次清掉的原文放回输入区。", StatusKind::Neutral);
  lastSource_.clear();
  updateControls();
}

// ---- 引擎回包 -------------------------------------------------------------

void PanelWindow::handleStatus(EngineStatus status) {
  const bool wasReady = engineStatus_ == EngineStatus::Ready;
  engineStatus_ = status;
  if (status == EngineStatus::Failed) {
    // 扣着的键全部按"没吃"放行；之后的键不再扣。
    for (HeldKey& held : held_) {
      held.resolved = true;
      held.eaten = false;
    }
    drainHeld();
    clearComposition();
    setStatus(L"输入引擎启动失败，按键将按英文原样输入。请查看程序目录 "
              L"user\\MoqiIM\\Log 下的日志。",
              StatusKind::Error);
  } else if (status == EngineStatus::Connecting) {
    // 新连接 = 新的引擎会话：旧会话的组字/候选已经不存在了。
    clearComposition();
    if (wasReady) {
      setStatus(L"与输入引擎的连接断开，正在重新连接…", StatusKind::Warn);
    }
  } else {
    setStatus(L"输入引擎就绪，可以打字了。", StatusKind::Success);
  }
  updateControls();
}

void PanelWindow::handleResult(EngineResult* raw) {
  std::unique_ptr<EngineResult> result(raw);
  if (result == nullptr) return;
  for (const Json::Value& frame : result->frames) applyFrame(frame);
  if (result->keyId != 0) resolveHeld(result->keyId, result->ok, result->eaten);
  if (result->isFlush) {
    if (result->flushed) {
      startTranslateRequest();
    } else if (translatePending_) {
      translatePending_ = false;
      setStatus(L"输入引擎连接中断，翻译已取消，输入区没有改动。", StatusKind::Error);
      updateControls();
    }
  }
}

void PanelWindow::applyFrame(const Json::Value& frame) {
  if (!frame.isObject()) return;
  const Json::Value& success = frame["success"];
  if (success.isBool() && !success.asBool()) return;  // 出错回执不带引擎状态
  const Json::Value& keys = frame["setSelKeys"];
  if (keys.isString()) {
    const std::wstring parsed = utf8ToWide(keys.asString());
    if (!parsed.empty()) selKeys_ = parsed;
  }
  const Json::Value& lang = frame["langStatus"];
  if (lang.isObject() && lang["ascii"].isBool()) setAsciiMode(lang["ascii"].asBool());
  const Json::Value& commit = frame["commitString"];
  if (commit.isString()) {
    const std::wstring text = utf8ToWide(commit.asString());
    if (!text.empty()) insertCommit(text);
  }
  const Json::Value& composition = frame["compositionString"];
  if (composition.isString()) {
    preedit_ = utf8ToWide(composition.asString());
    preeditCaret16_ = static_cast<int>(preedit_.size());
    const Json::Value& caret = frame["compositionCursor"];
    if (caret.isIntegral()) {
      preeditCaret16_ = utf16IndexFromCodePoints(preedit_, caret.asInt());
    }
  }
  const Json::Value& entries = frame["candidateEntries"];
  if (entries.isArray()) {
    candidates_.clear();
    for (const auto& entry : entries) {
      if (!entry.isObject()) continue;
      CandidateItem item;
      if (entry["text"].isString()) item.text = utf8ToWide(entry["text"].asString());
      if (entry["comment"].isString()) {
        item.comment = utf8ToWide(entry["comment"].asString());
      }
      candidates_.push_back(std::move(item));
    }
  }
  const Json::Value& show = frame["showCandidates"];
  if (show.isBool()) showCandidates_ = show.asBool();
  const Json::Value& cursor = frame["candidateCursor"];
  if (cursor.isIntegral()) highlighted_ = cursor.asInt();
  ::InvalidateRect(hwnd_, &stripRect_, FALSE);
}

void PanelWindow::setAsciiMode(bool ascii) {
  asciiMode_ = ascii;
  ::SetWindowTextW(modeButton_, ascii ? L"英" : L"中");
}

void PanelWindow::clearComposition() {
  preedit_.clear();
  preeditCaret16_ = 0;
  candidates_.clear();
  showCandidates_ = false;
  highlighted_ = 0;
  if (hwnd_ != nullptr) ::InvalidateRect(hwnd_, &stripRect_, FALSE);
}

void PanelWindow::insertCommit(const std::wstring& text) {
  // 上屏 = 在输入区光标处插入（有选区就替换选区，与正常打字一致）。走
  // EM_REPLACESEL 是为了进编辑框的撤销栈：Ctrl+Z 能撤回上屏。
  const std::wstring normalized = withCrLf(text);
  ::SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(normalized.c_str()));
  ::SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
}

// ---- 翻译 -----------------------------------------------------------------

void PanelWindow::doTranslate() {
  if (translating_ || translatePending_) {
    setStatus(L"正在翻译，请稍候。", StatusKind::Neutral);
    return;
  }
  const std::wstring text = trimmed(controlText(edit_));
  if (text.empty()) {
    setStatus(L"输入区是空的，没有可翻译的文字。", StatusKind::Warn);
    return;
  }
  const size_t count = codePointCount(text);
  if (count > kMaxTranslateCodePoints) {
    setStatus(L"超过 " + std::to_wstring(kMaxTranslateCodePoints) + L" 字（当前 " +
                  std::to_wstring(count) + L" 字），请分段翻译。",
              StatusKind::Warn);
    return;
  }
  TranslateConfig config;
  std::wstring loadError;
  if (!loadTranslateConfig(translateConfigPath_, &config, &loadError)) {
    setStatus(L"translate.json 有问题：" + loadError, StatusKind::Error);
    return;
  }
  if (!config.configured()) {
    setStatus(L"还没有配置翻译接口：点【翻译设置】填写接口地址和模型。", StatusKind::Warn);
    return;
  }
  if (!preedit_.empty()) {
    // 还有没上屏的拼音：先用空格顶屏（取当前高亮候选），别把它丢了。
    translatePending_ = true;
    setStatus(L"正在把组字上屏…", StatusKind::Neutral);
    engine_->postFlush();
    updateControls();
    return;
  }
  startTranslateRequest();
}

void PanelWindow::startTranslateRequest() {
  translatePending_ = false;
  // 记下原文在输入区里的位置（含它前面的空白一起清）。翻译期间输入区可能
  // 继续变，回来时只清这一段。
  const std::wstring raw = controlText(edit_);
  pendingSource_ = trimmed(raw);
  pendingLead_ = raw.find(pendingSource_);
  if (pendingLead_ == std::wstring::npos) pendingLead_ = 0;
  TranslateConfig config;
  std::wstring loadError;
  if (!loadTranslateConfig(translateConfigPath_, &config, &loadError) ||
      !config.configured()) {
    translating_ = false;
    setStatus(L"翻译接口配置读不出来，翻译已取消。", StatusKind::Error);
    updateControls();
    return;
  }
  translating_ = true;
  setStatus(L"正在翻译…", StatusKind::Neutral);
  updateControls();
  const std::string utf8 = wideToUtf8(pendingSource_);
  const HWND target = hwnd_;
  std::thread([target, config, utf8]() {
    auto* outcome = new TranslateOutcome(translateText(config, utf8));
    if (!::PostMessageW(target, WM_APP_TRANSLATE_DONE, 0,
                        reinterpret_cast<LPARAM>(outcome))) {
      delete outcome;
    }
  }).detach();
}

void PanelWindow::clearTranslatedSpan() {
  // 只清发出去的那一段原文（连同它前面的空白）：翻译期间继续打的字在它后面，
  // 原样留着。走选区替换是为了进撤销栈——Ctrl+Z 能把原文撤回来。
  const std::wstring current = controlText(edit_);
  const size_t span = pendingSource_.size();
  if (current.size() >= pendingLead_ + span &&
      current.compare(pendingLead_, span, pendingSource_) == 0) {
    ::SendMessageW(edit_, EM_SETSEL, 0, static_cast<LPARAM>(pendingLead_ + span));
    ::SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    return;
  }
  setStatus(L"译文已复制，但输入区的原文在翻译期间被改过，没有清空。", StatusKind::Warn);
}

void PanelWindow::handleTranslateDone(TranslateOutcome* raw) {
  std::unique_ptr<TranslateOutcome> outcome(raw);
  if (outcome == nullptr || !translating_) return;  // 迟到或重复的回包
  translating_ = false;
  if (!outcome->ok) {
    // 原子性：失败时输入区、结果区、剪贴板一律不动。
    setStatus(L"翻译失败，输入区没有改动：" + utf8ToWide(outcome->error), StatusKind::Error);
    updateControls();
    return;
  }
  const std::wstring translation = utf8ToWide(outcome->translation);
  const bool copied = copyText(hwnd_, convertPunctuation(translation));
  lastTranslation_ = translation;
  ::SetWindowTextW(resultSource_, pendingSource_.c_str());
  ::SetWindowTextW(resultTrans_, translation.c_str());
  if (!copied) {
    // 复制失败就不清屏：原文原样留在输入区，点【重新复制】即可。
    lastSource_.clear();
    setStatus(L"翻译成功但复制失败（剪贴板被别的程序占用）。点【重新复制】再试；"
              L"输入区没有清空。",
              StatusKind::Error);
  } else {
    clearTranslatedSpan();
    lastSource_ = pendingSource_;
    setStatus(L"已翻译并把译文复制到剪贴板（中文标点已换成英文标点）。", StatusKind::Success);
  }
  updateControls();
}

// ---- 窗口杂项 -------------------------------------------------------------

void PanelWindow::toggleTopMost(bool on) {
  topmost_ = on;
  ::SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ::SendMessageW(topmostCheck_, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void PanelWindow::openSettings() {
  TranslateConfig config;
  std::wstring loadError;
  loadTranslateConfig(translateConfigPath_, &config, &loadError);
  TranslateConfig saved;
  if (showSettingsDialog(hwnd_, uiFont_, loadError, config, &saved)) {
    if (saveTranslateConfig(translateConfigPath_, saved)) {
      setStatus(L"翻译设置已保存，下一次翻译即生效。", StatusKind::Success);
    } else {
      setStatus(L"翻译设置保存失败：程序目录不可写。", StatusKind::Error);
    }
  }
}

void PanelWindow::shutdown() {
  if (engine_ != nullptr) engine_->stop(2000);
}

} // namespace MoqiPanel
