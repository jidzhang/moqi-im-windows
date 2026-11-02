//
//	LocalEngine.cpp: in-process rime engine, phase 1 of the architecture
//	refactoring (docs/23-架构重构-引擎进程内化.md).
//
//	The per-key semantics ported here mirror the Go backend hot path
//	(moqi-ime/input_methods/rime/rime.go + rime_keyevent.go) so the two
//	paths stay interchangeable while the RPC fallback exists.
//
//	Locking: rime.dll is not concurrency-safe. Every public entry point of
//	LocalEngine holds RimeRuntime::mutex() for the whole call; all private
//	helpers assume it is already held.
//
#include "LocalEngine.h"
#include "MoqiTextService.h"
#include "MoqiImeModule.h"
#include "libIME2/src/Utils.h"
#include "KeyGuardLog.h"

#include <ShlObj.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace std;

namespace Moqi {

namespace {

// DLL 侧日志出口：RimeCore 生命周期事件进常开的 key-guard 日志。
void coreLogToGuardLog(const std::wstring& message) {
  appendRpcGuardLog(message);
}

struct CoreLogInit {
  CoreLogInit() { setCoreLogSink(&coreLogToGuardLog); }
};
const CoreLogInit g_coreLogInit;

// Ime::KeyEvent -> RimeCore 的解耦键值载荷。
KeyEventData keyDataOf(Ime::KeyEvent& e) {
  KeyEventData d;
  d.keyCode = e.keyCode();
  d.charCode = e.charCode();
  d.scanCode = e.scanCode();
  d.isExtended = e.isExtended();
  d.keyStates = e.keyStates();
  return d;
}

} // namespace

// ============================================================ LocalEngine ==

LocalEngine::LocalEngine(TextService* service) : service_(service) {}

bool LocalEngine::tryEngage() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready() || sessionId_ != 0) {
    return false;
  }
  // Never take over mid-composition: the RPC session owns that state.
  if (service_ != nullptr &&
      (service_->isComposing() || service_->showingCandidates())) {
    return false;
  }

  // Read shared appearance config (candidate count / second-selection /
  // shared mode state) — the same file the Go backend reads.
  Json::Value cfg;
  const wstring root = moqiAppDataDir();
  if (!root.empty()) {
    ifstream file(root + L"\\appearance_config.json", ifstream::binary);
    if (file) {
      file >> cfg;
    }
  }
  candidateCount_ = 7;
  if (cfg["candidate_count"].isInt() && cfg["candidate_count"].asInt() > 0) {
    candidateCount_ = cfg["candidate_count"].asInt();
  }
  semicolonSelectSecond_ =
      cfg.get("semicolon_select_second", false).asBool();

  std::lock_guard<std::mutex> lock(runtime.mutex());
  sessionId_ = runtime.rimeCreateSession();
  if (sessionId_ == 0) {
    return false;
  }
  // Mirror backend createSession(): apply shared input state, then page size.
  if (cfg.get("input_state_shared", false).asBool()) {
    const Json::Value& shared = cfg["shared_options"];
    if (shared.isObject()) {
      if (shared.isMember("ascii_mode")) {
        runtime.rimeSetOption(
            sessionId_, "ascii_mode",
            shared.get("ascii_mode", false).asBool() ? 1 : 0);
      }
      if (shared.isMember("full_shape")) {
        runtime.rimeSetOption(
            sessionId_, "full_shape",
            shared.get("full_shape", false).asBool() ? 1 : 0);
      }
    }
  }
  refreshPageSize();
  asciiBefore_ = runtime.rimeGetOption(sessionId_, "ascii_mode") != 0;
  fullBefore_ = runtime.rimeGetOption(sessionId_, "full_shape") != 0;
  asciiKnown_ = true;
  selectKeysCache_.clear();
  lastKeyDownCode_ = 0;
  lastKeySkip_ = 0;
  lastKeyUpCode_ = 0;
  keyComposing_ = false;
  secondSelectConsumeKeyUpCode_ = 0;
  rawInputTracked_.clear();
  rawInputAtCommit_.clear();
  displayPerm_.clear();
  // 配置热更新（contextual_lift.json mtime 变化时重载，TSV 只载一次）。
  RerankData::instance().ensureLoaded(runtime.sharedDataDirUtf8(),
                                      runtime.appDataRootUtf8());
  appendLocalEngineLog(L"engaged, session=" +
                       to_wstring(static_cast<uint64_t>(sessionId_)));
  return true;
}

void LocalEngine::deactivate() {
  if (sessionId_ == 0) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());
  runtime.rimeDestroySession(sessionId_);
  sessionId_ = 0;
  secondSelectConsumeKeyUpCode_ = 0;
  keyComposing_ = false;
}

bool LocalEngine::ensureSession() {
  if (sessionId_ != 0) {
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  sessionId_ = runtime.rimeCreateSession();
  return sessionId_ != 0;
}

// Sets menu/page_size to the configured candidate count (backend
// createSession(): SetCandidatePageSize when count != 9).
bool LocalEngine::refreshPageSize() {
  if (sessionId_ == 0 || candidateCount_ == 9) {
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  char schemaId[256] = {};
  if (!runtime.rimeGetCurrentSchema(sessionId_, schemaId, sizeof(schemaId))) {
    return false;
  }
  if (schemaId[0] == '\0') {
    return true;
  }
  rimeapi::Config config = {};
  if (!runtime.rimeSchemaOpen(schemaId, &config)) {
    return false;
  }
  const bool ok =
      runtime.rimeConfigSetInt(&config, "menu/page_size", candidateCount_) != 0;
  runtime.rimeConfigClose(&config);
  return ok;
}

LocalEngine::EngineState LocalEngine::currentState(bool* engineOk) {
  RimeRuntime& runtime = RimeRuntime::instance();
  EngineState state;
  *engineOk = sessionId_ != 0 && runtime.rimeFindSession(sessionId_) != 0;
  if (!*engineOk) {
    return state;
  }
  rimeapi::Commit commit = {};
  commit.data_size =
      static_cast<int>(sizeof(rimeapi::Commit) - sizeof(commit.data_size));
  if (runtime.rimeGetCommit(sessionId_, &commit)) {
    state.commit = commit.text != nullptr ? commit.text : "";
    runtime.rimeFreeCommit(&commit);
  }
  rimeapi::Context context = {};
  context.data_size = static_cast<int>(sizeof(rimeapi::Context) -
                                       sizeof(context.data_size));
  if (runtime.rimeGetContext(sessionId_, &context)) {
    state.composition = context.composition.preedit != nullptr
                            ? context.composition.preedit
                            : "";
    state.cursorPos = context.composition.cursor_pos;
    state.selStart = context.composition.sel_start;
    state.selEnd = context.composition.sel_end;
    state.highlightedIndex = context.menu.highlighted_candidate_index;
    state.selectKeys =
        context.menu.select_keys != nullptr ? context.menu.select_keys : "";
    const int count = context.menu.num_candidates;
    if (count > 0 && context.menu.candidates != nullptr) {
      state.candidates.reserve(count);
      for (int i = 0; i < count; ++i) {
        const rimeapi::Candidate& candidate = context.menu.candidates[i];
        state.candidates.emplace_back(
            candidate.text != nullptr ? candidate.text : "",
            candidate.comment != nullptr ? candidate.comment : "");
      }
    }
    runtime.rimeFreeContext(&context);
  }
  state.asciiMode = runtime.rimeGetOption(sessionId_, "ascii_mode") != 0;
  state.fullShape = runtime.rimeGetOption(sessionId_, "full_shape") != 0;
  return state;
}

// Port of snapshotRawInputForCommit: 提交类按键前保存 raw 快照（供
// typo_memory 记录；空格提交时跟踪值即将清零）。
void LocalEngine::snapshotRawInputForCommit() {
  if (!rawInputTracked_.empty()) {
    rawInputAtCommit_ = rawInputTracked_;
  }
  rawInputTracked_.clear();
}

// Port of updateTrackedRawInput：跟踪用户实际敲的字母串（a-z 与 '），
// 供错字记忆在提交时配对；keyCode/charCode 是重排换算后的有效键值。
void LocalEngine::updateTrackedRawInput(Ime::KeyEvent& e, UINT keyCode,
                                        UINT charCode, bool backendRet) {
  if (!backendRet) {
    return;
  }
  UINT ch = charCode;
  if (ch == 0 && keyCode >= 'A' && keyCode <= 'Z') {
    ch = keyCode + 32;
  }
  switch (keyCode) {
  case vkBack:
    if (!rawInputTracked_.empty()) {
      rawInputTracked_.pop_back(); // 跟踪串只含 ASCII
    }
    return;
  case vkSpace:
  case vkReturn:
    snapshotRawInputForCommit();
    return;
  case vkEscape:
    rawInputAtCommit_.clear();
    rawInputTracked_.clear();
    return;
  default:
    break;
  }
  const bool digit =
      (keyCode >= '1' && keyCode <= '9') || (ch >= '1' && ch <= '9');
  if (digit || (semicolonSelectSecond_ && isSecondSelectionShortcut(keyDataOf(e)))) {
    snapshotRawInputForCommit();
    return;
  }
  if (ch >= 'a' && ch <= 'z') {
    rawInputTracked_.push_back(static_cast<char>(ch));
    return;
  }
  if (ch == '\'' && !rawInputTracked_.empty() &&
      rawInputTracked_.back() != '\'') {
    rawInputTracked_.push_back('\'');
    return;
  }
  if (!rawInputTracked_.empty() && ch >= 0x20 && ch != '\'') {
    rawInputTracked_.clear();
  }
}

// Port of processKey(): translate, send to the engine, then the backend's
// special cases (arrow fallback, Return/Shift/CapsLock handling).
bool LocalEngine::processKey(Ime::KeyEvent& e, bool isUp, bool* ret) {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!ensureSession()) {
    *ret = false;
    return false;
  }
  if (!isUp) {
    bool engineOk = false;
    const EngineState state = currentState(&engineOk);
    keyComposing_ =
        engineOk && (!state.composition.empty() || !state.candidates.empty());
  }

  const bool isArrow = !isUp && (e.keyCode() == vkUp || e.keyCode() == vkDown);
  EngineState beforeState;
  if (isArrow) {
    bool engineOk = false;
    beforeState = currentState(&engineOk);
  }

  const int keycode0 = translateKeyCode(keyDataOf(e));
  const int mask = translateModifiers(keyDataOf(e), isUp);
  int keycode = keycode0;
  // 重排后的索引换算：显示序与引擎序不一致时，空格/数字指向显示位次，
  // 必须换算成引擎索引再发给引擎（否则按 2 选错词——历史实报教训）。
  UINT effKeyCode = e.keyCode();
  UINT effCharCode = e.charCode();
  if (!isUp && mask == 0 && keyComposing_ && !displayPerm_.empty()) {
    int target = -1;
    if (effKeyCode == vkSpace) {
      target = 0;
    } else if (effKeyCode >= '1' && effKeyCode <= '9') {
      target = static_cast<int>(effKeyCode) - '1';
    }
    if (target >= 0 && target < static_cast<int>(displayPerm_.size())) {
      const int engineIdx = displayPerm_[target];
      if (engineIdx != target && engineIdx >= 0 &&
          engineIdx < candidateCount_) {
        effKeyCode = static_cast<UINT>('1') + engineIdx;
        effCharCode = effKeyCode;
        keycode = static_cast<int>(effCharCode); // digit keysym = ASCII
      }
    }
  }
  bool backendRet = runtime.rimeProcessKey(sessionId_, keycode, mask) != 0;

  if (isArrow && mask == 0 && !beforeState.candidates.empty()) {
    // Arrow navigation fallback when the engine ignored Up/Down: move the
    // highlight ourselves (backend applyArrowNavigationFallback).
    bool engineOk = false;
    const EngineState afterState = currentState(&engineOk);
    if (!afterState.candidates.empty() &&
        afterState.highlightedIndex == beforeState.highlightedIndex) {
      int target =
          beforeState.highlightedIndex < 0 ? 0 : beforeState.highlightedIndex;
      if (e.keyCode() == vkUp && target > 0) {
        --target;
      } else if (e.keyCode() == vkDown &&
                 target < static_cast<int>(afterState.candidates.size()) - 1) {
        ++target;
      }
      if (target != afterState.highlightedIndex &&
          runtime.rimeHighlightCandidateOnCurrentPage != nullptr &&
          runtime.rimeHighlightCandidateOnCurrentPage(sessionId_, target)) {
        backendRet = true;
      }
    }
  }

  if (!isUp) {
    updateTrackedRawInput(e, effKeyCode, effCharCode, backendRet);
  }

  *ret = backendRet;
  if (!backendRet) {
    if (keyComposing_ && e.keyCode() == vkReturn) {
      *ret = true;
    } else if ((e.keyCode() == vkShift || e.keyCode() == vkCapital) &&
               (mask == 0 || mask == kReleaseMask)) {
      *ret = true;
    }
  }
  return true;
}

bool LocalEngine::readInputModes(bool* asciiMode, bool* fullShape) {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (sessionId_ == 0 || !runtime.rimeFindSession(sessionId_)) {
    return false;
  }
  *asciiMode = runtime.rimeGetOption(sessionId_, "ascii_mode") != 0;
  *fullShape = runtime.rimeGetOption(sessionId_, "full_shape") != 0;
  return true;
}

// Port of shouldPassThroughModifierOnKey().
bool LocalEngine::shouldPassThroughModifierOnKey(Ime::KeyEvent& e,
                                                 bool filterRet) {
  if (filterRet) {
    return false;
  }
  if (e.keyCode() == vkControl || e.keyCode() == vkMenu) {
    return true;
  }
  if (e.charCode() > 0 && e.charCode() < 0x20) {
    return true;
  }
  const BYTE* states = e.keyStates();
  return (states[vkControl] & 0x80) != 0 || (states[vkMenu] & 0x80) != 0;
}

// Port of updateLangStatus(): refresh mode button icons when the engine
// flipped ascii/full-shape (Shift toggle, schema switches).
void LocalEngine::appendLangStatusChange(Json::Value& resp, bool ascii,
                                         bool full) {
  if (!asciiKnown_) {
    asciiBefore_ = ascii;
    fullBefore_ = full;
    asciiKnown_ = true;
    return;
  }
  if (ascii == asciiBefore_ && full == fullBefore_) {
    return;
  }
  asciiBefore_ = ascii;
  fullBefore_ = full;

  const wstring& iconDir = RimeRuntime::instance().iconDir();
  if (iconDir.empty()) {
    return;
  }
  const bool capsOn = (::GetKeyState(VK_CAPITAL) & 1) != 0;
  auto iconPath = [&iconDir](const wchar_t* name) {
    return utf16ToUtf8((iconDir + L"\\" + name).c_str());
  };

  Json::Value changes(Json::arrayValue);
  const wstring mode = ascii ? L"eng" : L"chi";
  const wstring shape = full ? L"full" : L"half";
  const wstring caps = capsOn ? L"on" : L"off";
  Json::Value modeIcon;
  modeIcon["id"] = "windows-mode-icon";
  modeIcon["icon"] =
      iconPath((mode + L"_" + shape + L"_caps" + caps + L".ico").c_str());
  changes.append(modeIcon);
  Json::Value langIcon;
  langIcon["id"] = "switch-lang";
  langIcon["icon"] = iconPath(ascii ? L"eng.ico" : L"chi.ico");
  changes.append(langIcon);
  Json::Value shapeIcon;
  shapeIcon["id"] = "switch-shape";
  shapeIcon["icon"] = iconPath(full ? L"full.ico" : L"half.ico");
  changes.append(shapeIcon);
  resp["changeButton"] = changes;
}

// Port of fillResponseFromBackendState(): commit bookkeeping + rerank
// pipeline (contextual lift → bigram → mixed pair) + displayPerm + typo
// hoist, then composition/candidate fields.
void LocalEngine::buildResponse(const EngineState& state, bool allowCommit,
                                Json::Value& resp) {
  const RerankConfig& rerankCfg = RerankData::instance().config();
  if (allowCommit && !state.commit.empty()) {
    resp["commitString"] = state.commit;
    // recordTypoMemory：raw 优先取当前跟踪值，回退提交按键上的快照。
    string raw =
        rawInputTracked_.empty() ? rawInputAtCommit_ : rawInputTracked_;
    rawInputAtCommit_.clear();
    if (!raw.empty()) {
      TypoMemoryStore::instance().record(raw, state.commit);
    }
    contextualCommits_.push(state.commit, rerankCfg.windowSeconds);
    rawInputTracked_.clear();
  }
  Json::Value emptyList(Json::arrayValue);
  if (state.composition.empty()) {
    rawInputTracked_.clear();
    displayPerm_.clear();
    resp["compositionString"] = "";
    resp["candidateList"] = emptyList;
    resp["candidateEntries"] = emptyList;
    resp["showCandidates"] = false;
    return;
  }
  resp["compositionString"] = state.composition;
  const int caret =
      compositionCaretRuneIndex(state.composition, state.cursorPos);
  resp["cursorPos"] = caret;
  resp["compositionCursor"] = caret;
  resp["selStart"] = state.selStart;
  resp["selEnd"] = state.selEnd;

  size_t visible = state.candidates.size();
  if (candidateCount_ > 0 && visible > static_cast<size_t>(candidateCount_)) {
    visible = static_cast<size_t>(candidateCount_);
  }
  if (visible == 0) {
    displayPerm_.clear();
    resp["candidateList"] = emptyList;
    resp["candidateEntries"] = emptyList;
    resp["showCandidates"] = false;
    return;
  }

  // 截断后的候选进重排流水线（与 Go currentVisibleBackendState 一致）。
  vector<Cand> cands(state.candidates.begin(),
                     state.candidates.begin() + static_cast<long>(visible));
  int cursor = state.highlightedIndex;
  const vector<Cand> engineOrder = cands;
  string tail;
  if (contextualCommits_.activeTail(rerankCfg.windowSeconds, &tail)) {
    applyContextualLift(tail, cands, cursor);
    applyBigramRerank(tail, cands, cursor);
  }
  applyMixedPairRerank(state.composition, cands, cursor);
  displayPerm_ = buildDisplayPerm(engineOrder, cands);

  // 错字记忆提顶：同一 raw 上次选过的词已在候选里则提到第 1 位（仅改
  // 显示，displayPerm 已构建——与 Go 行为逐位一致）。
  if (!rawInputTracked_.empty() && cands.size() > 1) {
    const string remembered =
        TypoMemoryStore::instance().lookup(rawInputTracked_);
    if (!remembered.empty() && cands[0].first != remembered) {
      for (size_t i = 1; i < cands.size(); ++i) {
        if (cands[i].first == remembered) {
          const Cand item = cands[i];
          cands.erase(cands.begin() + static_cast<long>(i));
          cands.insert(cands.begin(), item);
          break;
        }
      }
    }
  }

  Json::Value entries(Json::arrayValue);
  for (const auto& cand : cands) {
    Json::Value item;
    item["text"] = cand.first;
    if (!cand.second.empty()) {
      item["comment"] = cand.second;
    }
    entries.append(item);
  }
  resp["candidateEntries"] = entries;
  if (cursor < 0) {
    cursor = 0;
  } else if (cursor >= static_cast<int>(cands.size())) {
    cursor = static_cast<int>(cands.size()) - 1;
  }
  resp["candidateCursor"] = cursor;
  resp["showCandidates"] = true;

  // Select keys follow the backend contract: only announce on change.
  string selectKeys =
      state.selectKeys.empty() ? "1234567890" : state.selectKeys;
  if (selectKeys.size() > cands.size()) {
    selectKeys.resize(cands.size());
  }
  if (selectKeys != selectKeysCache_) {
    resp["setSelKeys"] = selectKeys;
    selectKeysCache_ = selectKeys;
  }
}

bool LocalEngine::handleSecondSelectionFilterDown(Ime::KeyEvent& e,
                                                  Json::Value& resp) {
  if (!semicolonSelectSecond_ || !isSecondSelectionShortcut(keyDataOf(e))) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk || state.composition.empty() || state.candidates.size() < 2) {
    return false;
  }
  secondSelectConsumeKeyUpCode_ = selectionShortcutConsumeCode(keyDataOf(e));
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::handleSecondSelectionDown(Ime::KeyEvent& e,
                                            Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(keyDataOf(e)) != secondSelectConsumeKeyUpCode_) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk || state.composition.empty() || state.candidates.size() < 2) {
    buildResponse(state, false, resp);
    resp["returnValue"] = 1;
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  if (runtime.rimeSelectCandidateOnCurrentPage(sessionId_, 1)) {
    bool ok = false;
    const EngineState after = currentState(&ok);
    buildResponse(after, true, resp);
    appendLangStatusChange(resp, after.asciiMode, after.fullShape);
    resp["returnValue"] = 1;
    return true;
  }
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::handleSecondSelectionFilterUp(Ime::KeyEvent& e,
                                                Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(keyDataOf(e)) != secondSelectConsumeKeyUpCode_) {
    return false;
  }
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::handleSecondSelectionUp(Ime::KeyEvent& e,
                                          Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(keyDataOf(e)) != secondSelectConsumeKeyUpCode_) {
    return false;
  }
  secondSelectConsumeKeyUpCode_ = 0;
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::filterKeyDown(Ime::KeyEvent& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());

  if (handleSecondSelectionFilterDown(e, resp)) {
    return true;
  }
  // Same-key dedup (TSF duplicate events), with the bare-Shift exemption:
  // a lost keyup must never break the Shift toggle.
  if (!isBareShiftKeyCode(e.keyCode()) &&
      static_cast<int>(e.keyCode()) == lastKeyDownCode_) {
    ++lastKeySkip_;
    if (lastKeySkip_ >= 2) {
      lastKeyDownCode_ = 0;
      lastKeySkip_ = 0;
    }
  } else {
    lastKeyDownCode_ = static_cast<int>(e.keyCode());
    lastKeySkip_ = 0;
    if (!processKey(e, false, &lastKeyDownRet_)) {
      return false;
    }
    bool ascii = false;
    bool full = false;
    if (readInputModes(&ascii, &full)) {
      appendLangStatusChange(resp, ascii, full);
    }
  }
  lastKeyUpCode_ = 0;
  resp["returnValue"] = lastKeyDownRet_ ? 1 : 0;
  return true;
}

bool LocalEngine::onKeyDown(Ime::KeyEvent& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());

  if (handleSecondSelectionDown(e, resp)) {
    return true;
  }
  if (shouldPassThroughModifierOnKey(e, lastKeyDownRet_)) {
    resp["returnValue"] = 0;
    return true;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk) {
    resp["returnValue"] = 0;
    return true;
  }
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::filterKeyUp(Ime::KeyEvent& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());

  if (handleSecondSelectionFilterUp(e, resp)) {
    return true;
  }
  if (!isBareShiftKeyCode(e.keyCode()) &&
      static_cast<int>(e.keyCode()) == lastKeyUpCode_) {
    lastKeyUpCode_ = 0;
  } else if (isKeyReleaseRelevantForBackend(e.keyCode())) {
    lastKeyUpCode_ = static_cast<int>(e.keyCode());
    if (!processKey(e, true, &lastKeyUpRet_)) {
      return false;
    }
    bool ascii = false;
    bool full = false;
    if (readInputModes(&ascii, &full)) {
      appendLangStatusChange(resp, ascii, full);
    }
  } else {
    // Ordinary key releases never reach the engine (sentence-context rule).
    lastKeyUpCode_ = static_cast<int>(e.keyCode());
    lastKeyUpRet_ = false;
  }
  lastKeyDownCode_ = 0;
  lastKeySkip_ = 0;
  resp["returnValue"] = lastKeyUpRet_ ? 1 : 0;
  return true;
}

bool LocalEngine::onKeyUp(Ime::KeyEvent& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());

  if (handleSecondSelectionUp(e, resp)) {
    return true;
  }
  if (shouldPassThroughModifierOnKey(e, lastKeyUpRet_)) {
    resp["returnValue"] = 0;
    return true;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk) {
    resp["returnValue"] = 0;
    return true;
  }
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::highlightCandidate(int index, Json::Value& resp) {
  if (sessionId_ == 0 || index < 0 || index >= 9) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  lock_guard<mutex> lock(runtime.mutex());
  // Optional export in some rime.dll builds (upstream defines it static);
  // missing behaves like the Go procAvailable guard: not supported.
  if (runtime.rimeHighlightCandidateOnCurrentPage == nullptr ||
      !runtime.rimeHighlightCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  return true;
}

bool LocalEngine::selectCandidate(int index, Json::Value& resp) {
  if (sessionId_ == 0 || index < 0 || index >= 9) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());
  secondSelectConsumeKeyUpCode_ = 0;
  if (!runtime.rimeSelectCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  return true;
}

bool LocalEngine::changePage(bool backward, Json::Value& resp) {
  if (sessionId_ == 0) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  lock_guard<mutex> lock(runtime.mutex());
  if (runtime.rimeChangePage == nullptr ||
      !runtime.rimeChangePage(sessionId_, backward ? 1 : 0)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  return true;
}

bool LocalEngine::toggleOption(const char* name, Json::Value& resp) {
  if (sessionId_ == 0) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());
  if (!runtime.rimeFindSession(sessionId_)) {
    return false;
  }
  const bool current = runtime.rimeGetOption(sessionId_, name) != 0;
  runtime.rimeSetOption(sessionId_, name, current ? 0 : 1);
  bool ascii = false;
  bool full = false;
  if (readInputModes(&ascii, &full)) {
    appendLangStatusChange(resp, ascii, full);
  }
  return true;
}

void LocalEngine::onCompositionTerminated(bool forced) {
  secondSelectConsumeKeyUpCode_ = 0;
  keyComposing_ = false;
  rawInputTracked_.clear();
  displayPerm_.clear();
  if (sessionId_ == 0) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::mutex> lock(runtime.mutex());
  if (forced) {
    runtime.rimeDestroySession(sessionId_);
    sessionId_ = 0;
  } else {
    runtime.rimeClearComposition(sessionId_);
  }
}

} // namespace Moqi
