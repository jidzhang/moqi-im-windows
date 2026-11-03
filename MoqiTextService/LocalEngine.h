//
//	LocalEngine: in-process rime engine for the hot key path (architecture
//	refactoring phase 1, see docs/23-架构重构-引擎进程内化.md).
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU Library General Public License as
//	published by the Free Software Foundation; either version 2 of the
//	License, or (at your option) any later version.
//
#ifndef MOQI_LOCAL_ENGINE_H_
#define MOQI_LOCAL_ENGINE_H_

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <json/json.h>
#include "RimeCore.h"
#include "LocalRerank.h"

namespace Ime {
class KeyEvent;
}

namespace Moqi {

class TextService;

// Per-Client engine facade. Owns one rime session and implements the same
// per-key semantics as the Go backend's hot path (key translation, dedup,
// second-selection shortcut, modifier pass-through, response building), but
// emits Json responses in the shape Client's RPC apply path already
// consumes, so all display logic stays untouched.
//
// Phase-1 limitations (by design, see plan doc): no mixed_pair/bigram
// rerank, no typo memory, no custom phrase / super abbrev / AI / cloud
// clipboard overlays; user dictionary disabled in the in-proc user dir.
class LocalEngine {
public:
  explicit LocalEngine(TextService* service);

  // Take over the key path if the runtime is ready and no composition is
  // active. Returns true when this client now runs on the local engine.
  bool tryEngage();
  bool active() const { return sessionId_ != 0; }
  void deactivate();

  // All handlers return false only on a runtime failure (caller logs and
  // treats the key as not handled); resp follows the RPC Json contract.
  bool filterKeyDown(Ime::KeyEvent& e, Json::Value& resp);
  bool onKeyDown(Ime::KeyEvent& e, Json::Value& resp);
  bool filterKeyUp(Ime::KeyEvent& e, Json::Value& resp);
  bool onKeyUp(Ime::KeyEvent& e, Json::Value& resp);
  bool highlightCandidate(int index, Json::Value& resp);
  bool selectCandidate(int index, Json::Value& resp);
  bool changePage(bool backward, Json::Value& resp);
  void onCompositionTerminated(bool forced);
  // 引擎状态命令本地执行（ascii_mode/full_shape），响应带图标刷新。
  bool toggleOption(const char* name, Json::Value& resp);
  // 菜单"总候选数"变更的即时跟随（daemon ON_COMMAND 响应带
  // customizeUi.candidateCount）。
  void applyCandidateCount(int count);
  void setSemicolonSelectSecond(bool enabled) {
    semicolonSelectSecond_ = enabled;
  }

  // ---- AI 覆盖层（等价 Go fillAIResponse：只叠加第一条，原候选顺延）----
  // 外部进程（daemon）异步生成后经推送帧下发；DLL 只保管显示状态与选字
  // 映射，网络与模型调用仍全在外部进程。
  void setAiOverlay(const std::string& candidate);
  void clearAiOverlay();
  bool aiOverlayActive() const { return !aiOverlay_.empty(); }
  // 只按当前引擎状态重绘一次响应（不处理按键）——AI 覆盖层变化后刷新
  // 候选窗用。
  bool redraw(Json::Value& resp);
  // 导出当前组字与前几条候选（只读，不弹 commit）。AI 热键转发给 daemon
  // 时随请求携带：daemon 的 rime session 独立于本进程，看不到这里的组字。
  void getCompositionState(Json::Value& state);
  // 激活跟随：TSF onActivate 时重读共享输入状态/方案并套用（跨应用传播点，
  // 文件走 mtime 缓存，未启用时近零成本）。
  void refreshSyncedSettings();
  // 自上次导出后是否有提交（=userdb 有新学习）。Client 失焦时据此决定
  // 是否后台导出快照；导出成功后清零。
  bool consumeUserDictDirty() {
    bool d = userDictDirty_;
    userDictDirty_ = false;
    return d;
  }
  bool hasUserDictChanges() const { return userDictDirty_; }
  // 当前是否有组字（导出前的安全检查：RimeSyncUserData 会清所有会话）。
  bool hasComposition();
  // 导出学习快照并**原子重建会话**：RimeSyncUserData 内部 CleanupAllSessions
  // 会销毁全部会话（sessions_.clear()，2026-11-02 实证第二回合按键直通），
  // 必须在同一 runtime 锁内废句柄→导出→重建（ensureSession 重放共享态/方案），
  // 等锁的按键拿到手时已是好会话。返回是否有快照拷入主词典。
  // 会话不存在也能导（导出是进程级 userdb 操作）：反激活后仍可兜底导出。
  bool syncUserDbNow();
  // 反激活兜底导出是否可行（运行时已就绪且有本进程用户目录）。
  bool canExportUserDb();

private:
  // 跨进程学习导入：主词典 sync 有新快照时合并进本进程 userdb（会重建会话）。
  void maybeImportMasterLearning();

  struct EngineState {
    bool ok = false;
    std::string commit;
    std::string composition;
    int pageNo = 0;
    int cursorPos = 0;
    int selStart = 0;
    int selEnd = 0;
    int highlightedIndex = -1;
    std::string selectKeys;
    std::vector<std::pair<std::string, std::string>> candidates; // text, comment
    bool asciiMode = false;
    bool fullShape = false;
  };

  bool ensureSession();
  EngineState currentState(bool* engineOk);
  // Options-only probe for the filter phase: RimeGetCommit pops the commit
  // text, so it must never run before the onKeyDown response is built.
  bool readInputModes(bool* asciiMode, bool* fullShape);
  bool processKey(Ime::KeyEvent& e, bool isUp, bool* ret);
  void buildResponse(const EngineState& state, bool allowCommit,
                     Json::Value& resp);
  void appendLangStatusChange(Json::Value& resp, bool ascii, bool full);
  bool handleSecondSelectionFilterDown(Ime::KeyEvent& e, Json::Value& resp);
  bool handleSecondSelectionDown(Ime::KeyEvent& e, Json::Value& resp);
  bool handleSecondSelectionFilterUp(Ime::KeyEvent& e, Json::Value& resp);
  bool handleSecondSelectionUp(Ime::KeyEvent& e, Json::Value& resp);
  bool shouldPassThroughModifierOnKey(Ime::KeyEvent& e, bool filterRet);
  bool refreshPageSize();
  // Phase-2: raw-input tracking for typo memory (port of
  // updateTrackedRawInput) and the commit bookkeeping it feeds.
  void updateTrackedRawInput(Ime::KeyEvent& e, UINT keyCode, UINT charCode,
                             bool backendRet);
  void snapshotRawInputForCommit();

  TextService* service_;
  rimeapi::SessionId sessionId_ = 0;
  int candidateCount_ = 7;
  bool semicolonSelectSecond_ = false;
  int lastKeyDownCode_ = 0;
  int lastKeySkip_ = 0;
  int lastKeyUpCode_ = 0;
  bool lastKeyDownRet_ = false;
  bool lastKeyUpRet_ = false;
  bool keyComposing_ = false;
  int secondSelectConsumeKeyUpCode_ = 0;
  std::string selectKeysCache_;
  bool asciiKnown_ = false;
  bool asciiBefore_ = false;
  bool fullBefore_ = false;
  // Phase-2 rerank state (per client, mirrors the Go IME fields).
  ContextualCommits contextualCommits_;
  std::string rawInputTracked_;
  std::string rawInputAtCommit_;
  std::vector<int> displayPerm_; // display index -> engine index; empty = identity
  std::vector<std::string> aiOverlay_;
  int overlayCustomCount_ = 0;
  std::vector<std::string> overlayCustomTexts_;
  std::string overlaySuperText_;
  std::string overlayCommitText_;
  int overlayConsumeKeyUpCode_ = 0; // AI 覆盖：最多 1 条（Go 语义）
  std::string pendingAiCommit_;        // 待上屏的 AI 文本（下次响应带出）
  bool userDictDirty_ = false;         // 有提交待导出学习快照
};

} // namespace Moqi

#endif // MOQI_LOCAL_ENGINE_H_
