//
//	LocalEngine: 单引擎架构下已停用的进程内引擎占位。
//
//	2026-11-04 起 64 位应用统一走 MoqiDaemon（标准 Rime 用户目录 + 一次部署），
//	进程内引擎不再参与运行。这里保留同名空接口，隔离 MoqiClient 里的历史调用点；
//	后续可把调用点一并删除。
//
//	旧的 LocalEngine 实现在 git 历史里（删除前版本）。
//
#ifndef MOQI_LOCAL_ENGINE_H_
#define MOQI_LOCAL_ENGINE_H_

#include <json/json.h>

namespace Ime {
class KeyEvent;
}

namespace Moqi {

class TextService;

class LocalEngine {
public:
  explicit LocalEngine(TextService*) {}
  ~LocalEngine() = default;

  bool tryEngage() { return false; }
  bool active() const { return false; }
  void deactivate() {}

  bool filterKeyDown(Ime::KeyEvent&, Json::Value&) { return false; }
  bool onKeyDown(Ime::KeyEvent&, Json::Value&) { return false; }
  bool filterKeyUp(Ime::KeyEvent&, Json::Value&) { return false; }
  bool onKeyUp(Ime::KeyEvent&, Json::Value&) { return false; }
  bool highlightCandidate(int, Json::Value&) { return false; }
  bool selectCandidate(int, Json::Value&) { return false; }
  bool changePage(bool, Json::Value&) { return false; }
  void onCompositionTerminated(bool) {}
  bool toggleOption(const char*, Json::Value&) { return false; }
  void applyCandidateCount(int) {}
  void setSemicolonSelectSecond(bool) {}
  void setAiOverlay(const std::string&) {}
  void clearAiOverlay() {}
  bool aiOverlayActive() const { return false; }
  bool redraw(Json::Value&) { return false; }
  void getCompositionState(Json::Value&) {}
  void refreshSyncedSettings() {}
  bool consumeUserDictDirty() { return false; }
  bool hasUserDictChanges() const { return false; }
  bool hasComposition() { return false; }
  bool syncUserDbNow() { return false; }
  bool canExportUserDb() { return false; }
};

} // namespace Moqi

#endif // MOQI_LOCAL_ENGINE_H_
