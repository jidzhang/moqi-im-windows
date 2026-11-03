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
#include <set>
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
  // 新进程接管前先拉一次主词典的最新学习（跨进程学习共享）。
  maybeImportMasterLearning();

  // Read shared appearance config (candidate count / second-selection /
  // shared mode state) — the same file the Go backend reads.
  Json::Value cfg;
  const wstring root = moqiAppDataDir();
  if (!root.empty()) {
    ifstream file(root + L"\\appearance_config.json", ifstream::binary);
    if (file) {
      Json::CharReaderBuilder builder;
      std::string errs;
      if (!Json::parseFromStream(builder, file, &cfg, &errs)) {
        cfg = Json::Value();
      }
    }
  }
  candidateCount_ = 7;
  if (cfg["candidate_count"].isInt()) {
    const int n = cfg["candidate_count"].asInt();
    if (n == 3 || n == 5 || n == 7 || n == 9) candidateCount_ = n;
  }
  semicolonSelectSecond_ =
      cfg.get("semicolon_select_second", false).asBool();

  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
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
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
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
  if (sessionId_ != 0) {
    // Go createSession()：新会话套用输入状态共享 + 方案跟随（未启用时为
    // 空操作；文件读走 mtime 缓存）。
    applySharedInputState(sessionId_);
    const std::string schemaId = loadSyncedSchemaId();
    if (!schemaId.empty()) {
      char cur[256] = {};
      if (runtime.rimeGetCurrentSchema(sessionId_, cur, sizeof(cur)) &&
          schemaId != cur) {
        runtime.rimeSelectSchema(sessionId_, schemaId.c_str());
      }
    }
  }
  return sessionId_ != 0;
}

// 激活跟随：TSF onActivate 时刷新共享选项/方案（跨应用传播点——Go 靠
// 各 IME 实例激活时读文件，这里进程内会话同样在激活时重读）。
// 跨进程学习导入（2026-11-03）：主词典 sync/ 下出现比上次导入更新的快照时，
// 把其它进程学会的词合并进本进程 userdb。RimeSyncUserData 会清掉全部会话，
// 所以只在无组字窗口执行，并在返回后重建会话（调用方保证无组字）。
void LocalEngine::maybeImportMasterLearning() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready() || !runtime.rimeSyncUserData) {
    return;
  }
  const wstring userDir = runtime.inprocUserDir();
  const wstring root = moqiAppDataDir();
  if (userDir.empty() || root.empty()) return;
  const wstring masterSync = root + L"\\UserDictMaster\\sync";
  // 取主词典 sync/<子目录>/*.userdb.txt 的最新文件时间，而不是目录时间：
  //  p-*  = 各 64 位进程导出的学习快照
  //  <uuid> = daemon 自己的 Backup（32 位应用学会的词在这里）
  // 目录 mtime 对"覆盖同名文件"不更新（2026-11-03 实测：文件反复覆盖，
  // 目录时间不变，导入永远不触发），必须看文件。
  FILETIME newest = {};
  {
    WIN32_FIND_DATAW sub = {};
    HANDLE hSub = ::FindFirstFileW((masterSync + L"\\*").c_str(), &sub);
    if (hSub != INVALID_HANDLE_VALUE) {
      do {
        if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (sub.cFileName[0] == L'.') continue;
        WIN32_FIND_DATAW f2 = {};
        HANDLE hF2 = ::FindFirstFileW(
            (masterSync + L"\\" + sub.cFileName + L"\\*.userdb.txt").c_str(),
            &f2);
        if (hF2 == INVALID_HANDLE_VALUE) continue;
        do {
          if (CompareFileTime(&f2.ftLastWriteTime, &newest) > 0) {
            newest = f2.ftLastWriteTime;
          }
        } while (::FindNextFileW(hF2, &f2));
        ::FindClose(hF2);
      } while (::FindNextFileW(hSub, &sub));
      ::FindClose(hSub);
    }
  }
  if (newest.dwLowDateTime == 0 && newest.dwHighDateTime == 0) {
    return;
  }
  const wstring stamp = userDir + L"\\.master-import-stamp";
  WIN32_FILE_ATTRIBUTE_DATA stampStat = {};
  if (::GetFileAttributesExW(stamp.c_str(), GetFileExInfoStandard,
                             &stampStat) &&
      CompareFileTime(&newest, &stampStat.ftLastWriteTime) <= 0) {
    return; // 没有比上次导入更新的学习快照
  }
  if (hasComposition()) {
    return; // 组字中不能清会话；下次激活再试
  }
  const bool hadSession = sessionId_ != 0;
  {
    std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
    if (sessionId_ != 0) {
      runtime.rimeDestroySession(sessionId_);
      sessionId_ = 0;
    }
  }
  const bool imported = importMasterUserDbSnapshots();
  // 无论是否真的有新快照，都刷新时间戳，避免每次按键重复尝试。
  HANDLE h = ::CreateFileW(stamp.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    ::CloseHandle(h);
  }
  if (hadSession && sessionId_ == 0) {
    ensureSession();
  }
  if (imported) {
    appendLocalEngineLog(L"[userdb] cross-process learning imported");
  }
}

void LocalEngine::refreshSyncedSettings() {
  if (sessionId_ == 0) {
    return;
  }
  applySharedInputState(sessionId_);
  maybeImportMasterLearning();
  if (sessionId_ == 0) {
    return; // 导入过程重建过会话
  }
  // 总候选数也要在激活时重读：菜单改数字的那一刻本应用可能没有引擎接管
  // （或响应里没带上），已打开的应用切回来若不重读，引擎还停在旧值——
  // 窗口按新 perRow 换行画出旧总数的候选（用户实报"第一行5个第二行2个"）。
  {
    const wstring root = moqiAppDataDir();
    if (!root.empty()) {
      ifstream file(root + L"\\appearance_config.json", ifstream::binary);
      if (file) {
        Json::Value cfg;
        Json::CharReaderBuilder builder;
        std::string errs;
        if (!Json::parseFromStream(builder, file, &cfg, &errs)) {
          cfg = Json::Value();
        }
        if (cfg["candidate_count"].isInt() && cfg["candidate_count"].asInt() > 0) {
          applyCandidateCount(cfg["candidate_count"].asInt());
        }
      }
    }
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  const std::string schemaId = loadSyncedSchemaId();
  if (schemaId.empty()) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  if (runtime.rimeFindSession(sessionId_) == 0) {
    return;
  }
  char cur[256] = {};
  if (runtime.rimeGetCurrentSchema(sessionId_, cur, sizeof(cur)) &&
      schemaId != cur) {
    runtime.rimeSelectSchema(sessionId_, schemaId.c_str());
  }
}

// Sets menu/page_size to the configured candidate count (backend
// createSession(): SetCandidatePageSize). 9 也要显式写：从 5/7 改回 9 时
// 若跳过，schema 里残留的旧值会让 9 永远不生效。
bool LocalEngine::refreshPageSize() {
  if (sessionId_ == 0) {
    return true;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  char schemaId[256] = {};
  if (!runtime.rimeGetCurrentSchema(sessionId_, schemaId, sizeof(schemaId))) {
    return false;
  }
  // 刚创建的会话停在 ".default"：page_size 必须写在真正会被选用的方案配置
  // 上（否则写进 ".default" 的空 config，引擎按 default.yaml 的 9 翻页，而
  // 界面只显示 candidateCount_ 个，每页第 8、9 个候选永远不可达——"xin 没
  // 有鑫"实报）。
  std::string target = schemaId;
  if (target.empty() || target[0] == '.') {
    target = loadSyncedSchemaId();
    if (target.empty()) {
      target = "rime_frost";
    }
    if (runtime.rimeSelectSchema != nullptr) {
      runtime.rimeSelectSchema(sessionId_, target.c_str());
    }
  }
  rimeapi::Config config = {};
  if (!runtime.rimeSchemaOpen(target.c_str(), &config)) {
    return false;
  }
  const bool ok =
      runtime.rimeConfigSetInt(&config, "menu/page_size", candidateCount_) != 0;
  runtime.rimeConfigClose(&config);
  // Schema 在构造时缓存 page_size；改完 config 必须重建 Schema 才生效。
  if (ok && runtime.rimeSelectSchema != nullptr) {
    runtime.rimeSelectSchema(sessionId_, target.c_str());
    // ApplySchema 清瞬态选项，重放共享输入状态。
    applySharedInputState(sessionId_);
  }
  return ok;
}

// 菜单"总候选数"变更的即时生效：daemon 的 ON_COMMAND 响应带
// customizeUi.candidateCount，DLL 据此让进程内引擎立即跟随（Go 的
// applyCandidateCountConfig 等价物；否则用户要重启应用才见效）。
void LocalEngine::applyCandidateCount(int count) {
  if (count <= 0 || count == candidateCount_) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  candidateCount_ = count;
  refreshPageSize();
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
    state.pageNo = context.menu.page_no;
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
  // AI 覆盖层（Go handleAIKeyDown* 语义）：显示首位是 AI 结果。
  // 空格/回车/数字 1 → 提交 AI 文本；数字 k>1 → 引擎第 k-1 位（原候选顺延）；
  // 上下箭头只在 AI 条目内移动（仅一条 → 吞掉不动）；其他任何键（Esc、
  // 字母、退格、再按热键……）退出覆盖层，键照常交给引擎。
  if (!isUp && !aiOverlay_.empty()) {
    int target = -1;
    if (mask == 0) {
      if (effKeyCode == vkSpace || effKeyCode == vkReturn) {
        target = 0;
      } else if (effKeyCode >= '1' && effKeyCode <= '9') {
        target = static_cast<int>(effKeyCode) - '1';
      } else if (effKeyCode == vkUp || effKeyCode == vkDown) {
        *ret = true;
        return true;
      }
    }
    if (target == 0) {
      pendingAiCommit_ = aiOverlay_[0];
      aiOverlay_.clear();
      runtime.rimeClearComposition(sessionId_);
      *ret = true;
      return true;
    }
    if (target > 0) {
      // 顺延：AI 占首位，第 k 位对应引擎第 k-1 位。
      const int engineDigit = static_cast<int>('1') + (target - 1);
      effKeyCode = static_cast<UINT>(engineDigit);
      effCharCode = effKeyCode;
      keycode = engineDigit;
    } else {
      aiOverlay_.clear();
    }
  }
  if (!isUp && mask == 0 && keyComposing_ && !displayPerm_.empty()) {
    int target = -1;
    if (effKeyCode == vkSpace) {
      target = 0;
    } else if (effKeyCode >= '1' && effKeyCode <= '9') {
      target = static_cast<int>(effKeyCode) - '1';
    }
    if (target >= 0 && overlayCustomCount_ > 0) {
      target -= overlayCustomCount_;
    }
    if (target >= 0 && overlayCustomCount_ > 0 && displayPerm_.empty()) {
      effKeyCode = static_cast<UINT>('1') + target;
      effCharCode = effKeyCode;
      keycode = static_cast<int>(effCharCode);
    } else if (target >= 0 &&
               target < static_cast<int>(displayPerm_.size())) {
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

  // 输入状态共享：Shift 抬起 / Caps 按下后捕获四选项变化并落盘
  //（Go shouldSyncSharedInputStateAfterProcessKey；未启用时只付一次 stat）。
  syncSharedAfterKey(sessionId_, e.keyCode(), isUp);

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

void LocalEngine::setAiOverlay(const std::string& candidate) {
  aiOverlay_.clear();
  if (!candidate.empty()) {
    aiOverlay_.push_back(candidate);
  }
}

void LocalEngine::clearAiOverlay() { aiOverlay_.clear(); }

// 导出前的安全检查：RimeSyncUserData 会 CleanupAllSessions，组字中调用
// 会吃掉当前组字（内部持递归锁）。
bool LocalEngine::hasComposition() {
  Json::Value s;
  getCompositionState(s);
  return !s["composition"].asString().empty();
}

bool LocalEngine::syncUserDbNow() {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  // 导出本身是进程级 userdb 操作，不依赖会话。反激活后 sessionId_ 已是 0，
  // 若在这里早退，进程退出前最后一次学习就永远导不出去（2026-11-03 实报
  // "关掉记事本再打开就忘了"）。
  const bool hadSession = sessionId_ != 0;
  if (hadSession) {
    runtime.rimeDestroySession(sessionId_);  // 幂等：句柄可能已被清理
    sessionId_ = 0;
  }
  const int copied = syncInprocUserDbToMaster();
  if (hadSession) {
    // 立即重建（ensureSession 会重放输入状态共享/方案跟随）。
    ensureSession();
  }
  appendLocalEngineLog(L"[userdb] exported=" + to_wstring(copied) +
                       L", session rebuilt");
  return copied > 0;
}

bool LocalEngine::canExportUserDb() {
  RimeRuntime& runtime = RimeRuntime::instance();
  return runtime.ready() && !runtime.inprocUserDir().empty();
}

// 只重绘：按当前引擎状态生成一次响应（AI 覆盖层变化时刷候选窗）。
bool LocalEngine::redraw(Json::Value& resp) {
  if (sessionId_ == 0) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk) {
    return false;
  }
  resp["success"] = true;
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

void LocalEngine::getCompositionState(Json::Value& state) {
  state = Json::Value(Json::objectValue);
  state["composition"] = "";
  state["candidates"] = Json::Value(Json::arrayValue);
  if (sessionId_ == 0) {
    return;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  if (runtime.rimeFindSession(sessionId_) == 0) {
    return;
  }
  // 不走 currentState()：它会 RimeGetCommit 弹走待上屏文本。
  rimeapi::Context context = {};
  context.data_size = static_cast<int>(sizeof(rimeapi::Context) -
                                       sizeof(context.data_size));
  if (!runtime.rimeGetContext(sessionId_, &context)) {
    return;
  }
  state["composition"] = context.composition.preedit != nullptr
                             ? context.composition.preedit
                             : "";
  const int count = context.menu.num_candidates;
  // AI 提示只用前 3 条（与 Go collectAICandidateTexts 一致）。
  for (int i = 0; i < count && i < 3 && context.menu.candidates != nullptr;
       ++i) {
    const char* text = context.menu.candidates[i].text;
    state["candidates"].append(text != nullptr ? text : "");
  }
  runtime.rimeFreeContext(&context);
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
    userDictDirty_ = true;  // 有提交=有学习，失焦时导出快照汇入主词典
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
    overlayCustomCount_ = 0;
    overlayCustomTexts_.clear();
    overlaySuperText_.clear();
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

  InputOverlay overlay;
  if (aiOverlay_.empty()) {
    overlay = computeInputOverlay(rawInputTracked_, state.composition,
                                  state.pageNo, candidateCount_);
  }
  overlayCustomCount_ = static_cast<int>(overlay.custom.size());
  overlayCustomTexts_.clear();
  for (const auto& c : overlay.custom) overlayCustomTexts_.push_back(c.text);
  overlaySuperText_ = overlay.superText;
  const bool superSynthetic = overlay.custom.empty() &&
                              !overlay.superText.empty() &&
                              state.candidates.empty();

  size_t visible = state.candidates.size();
  if (candidateCount_ > 0 && visible > static_cast<size_t>(candidateCount_)) {
    visible = static_cast<size_t>(candidateCount_);
  }
  if (visible == 0 && overlay.custom.empty() && !superSynthetic) {
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
          // 2026-11-02 修复：提顶改了显示顺序，displayPerm 必须同步旋转
          //（否则空格/数字仍按提顶前的映射选——显示"是嘛"上屏"事吗"，
          // 用户实报 shima）。buildDisplayPerm 在"同序"时返回空（=恒等
          // 映射），提顶恰恰打破恒等，必须显式生成旋转后的映射。
          if (displayPerm_.empty()) {
            displayPerm_.resize(cands.size());
            for (size_t k = 0; k < cands.size(); ++k) {
              displayPerm_[k] = static_cast<int>(k);
            }
          }
          if (i < displayPerm_.size()) {
            const int picked = displayPerm_[i];
            displayPerm_.erase(displayPerm_.begin() + static_cast<long>(i));
            displayPerm_.insert(displayPerm_.begin(), picked);
          }
          break;
        }
      }
    }
  }

  Json::Value entries(Json::arrayValue);
  // AI 覆盖层（等价 Go fillAIResponse）：只把第一条 AI 结果放在显示首位，
  // 原候选顺延；光标停在首位。按热键再触发时由 clearAiOverlay 切回原候选。
  if (!aiOverlay_.empty()) {
    Json::Value item;
    item["text"] = aiOverlay_[0];
    entries.append(item);
  }
  std::set<std::string> entryTexts;
  for (size_t i = 0; i < overlay.custom.size(); ++i) {
    Json::Value item;
    item["text"] = overlay.custom[i].text;
    if (i == 0 && !overlay.superText.empty()) {
      item["comment"] = overlay.superText;
    }
    entries.append(item);
    entryTexts.insert(overlay.custom[i].text);
  }
  if (overlay.custom.empty() && !overlay.superText.empty() &&
      !cands.empty()) {
    cands[0].second = overlay.superText;
  }
  const bool superOnly = overlay.custom.empty() && superSynthetic;
  if (superOnly) {
    Json::Value item;
    item["text"] = overlay.superText;
    item["comment"] = "?";
    entries.append(item);
    entryTexts.insert(overlay.superText);
  }
  for (const auto& cand : cands) {
    if (!entryTexts.insert(cand.first).second) continue;
    if (candidateCount_ > 0 &&
        static_cast<int>(entries.size()) >= candidateCount_) {
      break;
    }
    Json::Value item;
    item["text"] = cand.first;
    if (!cand.second.empty()) {
      item["comment"] = cand.second;
    }
    entries.append(item);
  }
  resp["candidateEntries"] = entries;
  if (!aiOverlay_.empty()) {
    cursor = 0; // 与 Go 一致：AI 结果落在首位并高亮
  } else if (!overlay.custom.empty() || superOnly) {
    cursor = 0;
  } else if (cursor < 0) {
    cursor = 0;
  } else if (cursor >= static_cast<int>(entries.size())) {
    cursor = entries.empty() ? 0 : static_cast<int>(entries.size()) - 1;
  }
  resp["candidateCursor"] = cursor;
  resp["showCandidates"] = true;

  // AI 覆盖时显示项数 = 1 + 原候选数，截断到 candidateCount_。
  if (!aiOverlay_.empty()) {
    Json::Value trimmed(Json::arrayValue);
    const int limit = candidateCount_ > 0 ? candidateCount_ : 7;
    for (Json::ArrayIndex i = 0;
         i < entries.size() && static_cast<int>(i) < limit; ++i) {
      trimmed.append(entries[i]);
    }
    resp["candidateEntries"] = trimmed;
  }

  // Select keys follow the backend contract: only announce on change.
  string selectKeys =
      state.selectKeys.empty() ? "1234567890" : state.selectKeys;
  if (selectKeys.size() > entries.size()) {
    selectKeys.resize(entries.size());
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
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

  // 置顶短语 / 超级简拼（Go custom_phrase/super_abbrev）。
  if (aiOverlay_.empty() &&
      (overlayCustomCount_ > 0 || !overlaySuperText_.empty())) {
    const bool superKey = e.keyCode() == VK_TAB ||
                          e.keyCode() == VK_OEM_PERIOD || e.charCode() == '.';
    if (superKey && !overlaySuperText_.empty()) {
      overlayCommitText_ = overlaySuperText_;
      overlayConsumeKeyUpCode_ = e.keyCode() == VK_TAB ? VK_TAB : '.';
      lastKeyDownRet_ = true;
      lastKeyUpCode_ = 0;
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode() == vkSpace && overlayCustomCount_ > 0) {
      overlayCommitText_ = overlayCustomTexts_[0];
      overlayConsumeKeyUpCode_ = vkSpace;
      lastKeyDownRet_ = true;
      resp["returnValue"] = 1;
      return true;
    }
    if (e.keyCode() >= '1' && e.keyCode() <= '9') {
      const int index = static_cast<int>(e.keyCode()) - '1';
      if (index < overlayCustomCount_) {
        overlayCommitText_ = overlayCustomTexts_[index];
        overlayConsumeKeyUpCode_ = static_cast<int>(e.keyCode());
        lastKeyDownRet_ = true;
        resp["returnValue"] = 1;
        return true;
      }
    }
  }

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
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

  if (!overlayCommitText_.empty()) {
    const std::string text = overlayCommitText_;
    overlayCommitText_.clear();
    overlayConsumeKeyUpCode_ = 0;
    if (sessionId_ != 0 && runtime.rimeClearComposition != nullptr) {
      runtime.rimeClearComposition(sessionId_);
    }
    keyComposing_ = false;
    rawInputTracked_.clear();
    rawInputAtCommit_.clear();
    displayPerm_.clear();
    overlayCustomCount_ = 0;
    overlayCustomTexts_.clear();
    overlaySuperText_.clear();
    resp["returnValue"] = 1;
    resp["commitString"] = text;
    resp["compositionString"] = "";
    resp["candidateList"] = Json::Value(Json::arrayValue);
    resp["candidateEntries"] = Json::Value(Json::arrayValue);
    resp["showCandidates"] = false;
    return true;
  }

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
  // AI 覆盖层选中上屏（Go commitAICandidate 等价）：清组字 + 直接上屏。
  if (!pendingAiCommit_.empty()) {
    resp["commitString"] = pendingAiCommit_;
    resp["compositionString"] = "";
    resp["candidateList"] = Json::Value(Json::arrayValue);
    resp["candidateEntries"] = Json::Value(Json::arrayValue);
    resp["showCandidates"] = false;
    pendingAiCommit_.clear();
  }
  return true;
}

bool LocalEngine::filterKeyUp(Ime::KeyEvent& e, Json::Value& resp) {
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

  if (overlayConsumeKeyUpCode_ != 0 &&
      (static_cast<int>(e.keyCode()) == overlayConsumeKeyUpCode_ ||
       (overlayConsumeKeyUpCode_ == '.' &&
        (e.keyCode() == VK_OEM_PERIOD || e.charCode() == '.')))) {
    overlayConsumeKeyUpCode_ = 0;
    resp["returnValue"] = 1;
    return true;
  }

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
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());

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
  lock_guard<recursive_mutex> lock(runtime.mutex());
  // Optional export in some rime.dll builds (upstream defines it static);
  // missing behaves like the Go procAvailable guard: not supported.
  if (runtime.rimeHighlightCandidateOnCurrentPage == nullptr ||
      !runtime.rimeHighlightCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::selectCandidate(int index, Json::Value& resp) {
  if (aiOverlay_.empty() && overlayCustomCount_ > 0 && index >= 0 &&
      index < overlayCustomCount_) {
    const std::string text = overlayCustomTexts_[index];
    RimeRuntime& rt = RimeRuntime::instance();
    std::lock_guard<std::recursive_mutex> lock(rt.mutex());
    if (sessionId_ != 0 && rt.rimeClearComposition != nullptr) {
      rt.rimeClearComposition(sessionId_);
    }
    keyComposing_ = false;
    rawInputTracked_.clear();
    displayPerm_.clear();
    overlayCustomCount_ = 0;
    overlayCustomTexts_.clear();
    overlaySuperText_.clear();
    resp["returnValue"] = 1;
    resp["commitString"] = text;
    resp["compositionString"] = "";
    resp["candidateList"] = Json::Value(Json::arrayValue);
    resp["candidateEntries"] = Json::Value(Json::arrayValue);
    resp["showCandidates"] = false;
    return true;
  }
  if (aiOverlay_.empty() && overlayCustomCount_ > 0 && index >= 0) {
    index -= overlayCustomCount_;
  }
  if (sessionId_ == 0 || index < 0 || index >= 9) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  secondSelectConsumeKeyUpCode_ = 0;
  if (!runtime.rimeSelectCandidateOnCurrentPage(sessionId_, index)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, true, resp);
  appendLangStatusChange(resp, state.asciiMode, state.fullShape);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::changePage(bool backward, Json::Value& resp) {
  if (sessionId_ == 0) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  lock_guard<recursive_mutex> lock(runtime.mutex());
  int changed = 0;
  if (runtime.rimeChangePage != nullptr) {
    changed = runtime.rimeChangePage(sessionId_, backward ? 1 : 0);
  }
  if (!changed && runtime.rimeProcessKey != nullptr) {
    // RimeChangePage 在定制 dll 里是 static（仅 RimeApi 表），平铺导出
    // 不存在；用 Page_Up/Page_Down keysym 走 navigator 同一条路径。
    changed = runtime.rimeProcessKey(sessionId_, backward ? kRimePrior : kRimeNext, 0);
  }
  if (!changed) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  buildResponse(state, false, resp);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::toggleOption(const char* name, Json::Value& resp) {
  if (sessionId_ == 0) {
    return false;
  }
  RimeRuntime& runtime = RimeRuntime::instance();
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  if (!runtime.rimeFindSession(sessionId_)) {
    return false;
  }
  const bool current = runtime.rimeGetOption(sessionId_, name) != 0;
  runtime.rimeSetOption(sessionId_, name, current ? 0 : 1);
  // 共享四键被菜单/按钮翻转 → 捕获落盘（Go toggleOption:shared_option:*）。
  syncSharedAfterOptionToggle(sessionId_);
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
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  if (forced) {
    runtime.rimeDestroySession(sessionId_);
    sessionId_ = 0;
  } else {
    runtime.rimeClearComposition(sessionId_);
  }
}

} // namespace Moqi
