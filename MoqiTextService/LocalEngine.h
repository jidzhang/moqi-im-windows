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

private:
  struct EngineState {
    bool ok = false;
    std::string commit;
    std::string composition;
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
};

} // namespace Moqi

#endif // MOQI_LOCAL_ENGINE_H_
