//
//	RimeCore: engine core shared by the TSF DLL (LocalEngine) and the
//	C++ helper process (moqi-helper). Owns the rime.dll flat-API loader,
//	the in-proc user-dir/deployment layout, key translation, and the
//	pure helpers both sides need. No TSF/libIME2 dependency on purpose.
//
#ifndef MOQI_RIME_CORE_H_
#define MOQI_RIME_CORE_H_

#include <windows.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <json/json.h>

#include "JsonSafeRead.h"

namespace Moqi {

// Self-contained mirror of librime's flat C API (src/rime_api.h types +
// exported Rime* wrappers). Kept in sync with the rime.dll we ship; struct
// versioning via data_size tolerates additive upstream changes.
namespace rimeapi {

typedef uintptr_t SessionId;

// RimeGetSchemaList（librime rime_schema_list_t，字段顺序照抄 rime_api.h）
typedef struct {
  char* schema_id;
  char* name;
  void* reserved;
} RimeSchemaListItem;
typedef struct {
  size_t size;
  RimeSchemaListItem* list;
} RimeSchemaList;

typedef struct {
  int data_size;
  const char* shared_data_dir;
  const char* user_data_dir;
  const char* distribution_name;
  const char* distribution_code_name;
  const char* distribution_version;
  const char* app_name;
  const char** modules;
  int min_log_level;
  const char* log_dir;
  const char* prebuilt_data_dir;
  const char* staging_dir;
} Traits;

typedef struct {
  int length;
  int cursor_pos;
  int sel_start;
  int sel_end;
  char* preedit;
} Composition;

typedef struct {
  char* text;
  char* comment;
  void* reserved;
} Candidate;

typedef struct {
  int page_size;
  int page_no;
  int is_last_page;
  int highlighted_candidate_index;
  int num_candidates;
  Candidate* candidates;
  char* select_keys;
} Menu;

typedef struct {
  int data_size;
  char* text;
} Commit;

typedef struct {
  int data_size;
  Composition composition;
  Menu menu;
  char* commit_text_preview;
  char** select_labels;
} Context;

typedef struct {
  void* ptr;
} Config;

} // namespace rimeapi

// Process-wide rime.dll runtime: LoadLibrary + one-shot setup/initialize/
// maintenance on a background thread. rime.dll is not concurrency-safe, so
// every API call must be made while holding mutex().
class RimeRuntime {
public:
  static RimeRuntime& instance();

  // Phase-1 gates: x64 process (we only ship an x64 rime.dll) and no
  // MOQI_LOCAL_ENGINE=0 override.
  bool supported();

  // Idempotent. Kicks a background thread that loads rime.dll, seeds the
  // in-proc user dir and runs deployment. ready() flips true on success.
  // dllDir：rime.dll 的查找目录，缺省 = rimeDir（programDir\backendDir\
  // input_methods\rime）。x86 MoqiDaemon 传自己的目录——同一安装里
  // moqi-ime\...\rime.dll 是 x64（Go 后端/x64 TSF DLL 用），x86 版放安装根。
  // masterUserDir（2026-11-02 学习闭环）：非空 = daemon 主词典模式（独占
  // 目录、userdb 开启、汇入各进程学习快照）；空 = DLL 每进程 p-<hex> 模式。
  void requestInit(const std::wstring& programDir,
                   const std::wstring& backendDir,
                   const std::wstring& dllDir = L"",
                   const std::wstring& masterUserDir = L"");

  bool ready() const { return state_ == State::Ready; }
  bool failed() const { return state_ == State::Failed; }

  // 首键等待引擎就绪（首次部署/冷启动期间按键不能直落英文）。
  // 返回 true = 已就绪；false = 超时或初始化失败。
  bool waitUntilReady(int timeoutMs);

  // 本进程的进程内引擎用户目录（requestInit 时确定；p-<hex> 每进程唯一）。
  const std::wstring& inprocUserDir() const { return inprocUserDir_; }

  // rime.dll 非线程安全但同线程可重入（librime 内部无 TLS 假设）；输入
  // 状态共享等组合路径会嵌套加锁，故用递归锁（2026-11-02）。
  std::recursive_mutex& mutex() { return apiMutex_; }

  // Flat API procs; all guaranteed non-null once ready() (the two optional
  // exports stay null when the dll does not provide them).
  void (*rimeSetup)(rimeapi::Traits*) = nullptr;
  void (*rimeInitialize)(rimeapi::Traits*) = nullptr;
  int (*rimeStartMaintenance)(int fullCheck) = nullptr;
  void (*rimeJoinMaintenanceThread)() = nullptr;
  // 同步用户词典（导出快照到 sync/<installation>/ 并合并 sync/*/ 的外来
  // 快照；见 user_dict_manager.cc Synchronize）。调用会 CleanupAllSessions，
  // 只能在无组字的空闲窗口调用并 join 等完成。
  int (*rimeSyncUserData)() = nullptr;
  rimeapi::SessionId (*rimeCreateSession)() = nullptr;
  int (*rimeFindSession)(rimeapi::SessionId) = nullptr;
  void (*rimeDestroySession)(rimeapi::SessionId) = nullptr;
  int (*rimeProcessKey)(rimeapi::SessionId, int keycode, int mask) = nullptr;
  void (*rimeClearComposition)(rimeapi::SessionId) = nullptr;
  int (*rimeGetCommit)(rimeapi::SessionId, rimeapi::Commit*) = nullptr;
  void (*rimeFreeCommit)(rimeapi::Commit*) = nullptr;
  int (*rimeGetContext)(rimeapi::SessionId, rimeapi::Context*) = nullptr;
  void (*rimeFreeContext)(rimeapi::Context*) = nullptr;
  void (*rimeSetOption)(rimeapi::SessionId, const char*, int) = nullptr;
  int (*rimeGetOption)(rimeapi::SessionId, const char*) = nullptr;
  int (*rimeSelectCandidateOnCurrentPage)(rimeapi::SessionId, int) = nullptr;
  int (*rimeHighlightCandidateOnCurrentPage)(rimeapi::SessionId, int) = nullptr;
  int (*rimeChangePage)(rimeapi::SessionId, int backward) = nullptr;
  int (*rimeGetCurrentSchema)(rimeapi::SessionId, char*, size_t) = nullptr;
  int (*rimeGetSchemaList)(rimeapi::RimeSchemaList*) = nullptr;
  void (*rimeFreeSchemaList)(rimeapi::RimeSchemaList*) = nullptr;
  int (*rimeSelectSchema)(rimeapi::SessionId, const char*) = nullptr;
  int (*rimeSchemaOpen)(const char*, rimeapi::Config*) = nullptr;
  int (*rimeConfigClose)(rimeapi::Config*) = nullptr;
  int (*rimeConfigSetInt)(rimeapi::Config*, const char*, int) = nullptr;

  const std::wstring& iconDir() const { return iconDir_; }
  // UTF-8 dirs for the rerank data (set by the init thread).
  const std::string& sharedDataDirUtf8() const { return sharedDataDirUtf8_; }
  const std::string& appDataRootUtf8() const { return appDataRootUtf8_; }

private:
  RimeRuntime() = default;
  bool loadLibrary(const std::wstring& rimeDir);

  enum class State { Idle, Loading, Ready, Failed };
  std::mutex stateMutex_; // guards state_ transitions + init thread spawn
  State state_ = State::Idle;
  std::wstring inprocUserDir_; // requestInit 填充（p-<hex>）
  std::recursive_mutex apiMutex_; // serializes all rime API calls in this process
  HMODULE module_ = nullptr;
  std::wstring iconDir_;
  std::string sharedDataDirUtf8_;
  std::string appDataRootUtf8_;
};

// 日志出口：宿主注入（DLL -> key-guard 日志；Helper -> 自有文件）。
void setCoreLogSink(void (*sink)(const std::wstring&));
void coreLog(const std::wstring& message);

std::string wideToUtf8(const wchar_t* text);
std::wstring utf8ToWide(const char* text);

// ---------------------------------------------------------------- paths --

std::wstring moqiAppDataDir();      // %APPDATA%\Moqi
std::wstring localAppDataDirW();    // %LOCALAPPDATA%

// 共享用户配置目录（%APPDATA%\Moqi\<方案集>，默认 Rime）。用户把 schema、
// 词库、gram 放这里；引擎初始化时以硬链接投影进各自 user_data_dir，既让所有
// 进程看到同一份配置，又保留 userdb 的进程独占。
std::wstring sharedUserConfigDir();
// scheme_set.json {"current": name} with the backend's defaults.
std::wstring currentSchemeSetDirName();

std::string readFileUtf8(const std::wstring& path);
void writeFileUtf8(const std::wstring& path, const std::string& content);

// In-proc rime_frost.custom.yaml: backend patch lines verbatim (engine
// behavior parity). 阶段3注：用户词典开启（多进程共享实证，见 24-交接）。
std::string buildInProcCustomPatch(const std::wstring& backendUserDir,
                                   const std::wstring& sharedDataDir = L"");

// --------------------------------------------------- 置顶短语/超级简拼 ----
// Go custom_phrase.go / super_abbrev.go 的轻量等价物：文件解析、前缀匹配、
// 排序与 super abbrev 查表。引擎层负责把结果前置到候选列表、按键选择。
struct OverlayCandidate {
  std::string text;
  std::string comment;
};
struct InputOverlay {
  std::vector<OverlayCandidate> custom;  // 置顶短语（已按 Go 规则排序去重）
  std::string superText;                 // 超级简拼映射文本（空=无）
};
// pageNo > 0 时 Go 会清掉置顶短语 overlay；rawInput 为空时回退 composition。
InputOverlay computeInputOverlay(const std::string& rawInput,
                                 const std::string& composition, int pageNo,
                                 int limit);

constexpr int kVoidSymbol = 0xFFFFFF;
constexpr int kRimeBackSpace = 0xFF08;
constexpr int kRimeTab = 0xFF09;
constexpr int kRimeClear = 0xFF0B;
constexpr int kRimeReturn = 0xFF0D;
constexpr int kRimePause = 0xFF13;
constexpr int kRimeCapsLock = 0xFFE5;
constexpr int kRimeEscape = 0xFF1B;
constexpr int kRimePrior = 0xFF55;
constexpr int kRimeNext = 0xFF56;
constexpr int kRimeEnd = 0xFF57;
constexpr int kRimeHome = 0xFF50;
constexpr int kRimeLeft = 0xFF51;
constexpr int kRimeUp = 0xFF52;
constexpr int kRimeRight = 0xFF53;
constexpr int kRimeDown = 0xFF54;
constexpr int kRimeSelect = 0xFF60;
constexpr int kRimePrint = 0xFF61;
constexpr int kRimeExecute = 0xFF62;
constexpr int kRimeInsert = 0xFF63;
constexpr int kRimeDelete = 0xFFFF;
constexpr int kRimeHelp = 0xFF6A;
constexpr int kRimeMetaL = 0xFFE7;
constexpr int kRimeMetaR = 0xFFE8;
constexpr int kRimeNumLock = 0xFF7F;
constexpr int kRimeScrollLock = 0xFF14;
constexpr int kRimeShiftL = 0xFFE1;
constexpr int kRimeShiftR = 0xFFE2;
constexpr int kRimeControlL = 0xFFE3;
constexpr int kRimeControlR = 0xFFE4;
constexpr int kRimeAltL = 0xFFE9;
constexpr int kRimeAltR = 0xFFEA;
constexpr int kRimeF1 = 0xFFBE;

constexpr int kShiftMask = 1 << 0;
constexpr int kLockMask = 1 << 1;
constexpr int kControlMask = 1 << 2;
constexpr int kAltMask = 1 << 3;
constexpr int kReleaseMask = 1 << 30;

constexpr UINT vkBack = 0x08;
constexpr UINT vkTab = 0x09;
constexpr UINT vkClear = 0x0C;
constexpr UINT vkReturn = 0x0D;
constexpr UINT vkShift = 0x10;
constexpr UINT vkControl = 0x11;
constexpr UINT vkMenu = 0x12;
constexpr UINT vkPause = 0x13;
constexpr UINT vkCapital = 0x14;
constexpr UINT vkEscape = 0x1B;
constexpr UINT vkSpace = 0x20;
constexpr UINT vkPrior = 0x21;
constexpr UINT vkNext = 0x22;
constexpr UINT vkEnd = 0x23;
constexpr UINT vkHome = 0x24;
constexpr UINT vkLeft = 0x25;
constexpr UINT vkUp = 0x26;
constexpr UINT vkRight = 0x27;
constexpr UINT vkDown = 0x28;
constexpr UINT vkSelect = 0x29;
constexpr UINT vkPrint = 0x2A;
constexpr UINT vkExecute = 0x2B;
constexpr UINT vkInsert = 0x2D;
constexpr UINT vkDelete = 0x2E;
constexpr UINT vkHelp = 0x2F;
constexpr UINT vkLWin = 0x5B;
constexpr UINT vkRWin = 0x5C;
constexpr UINT vkNumLock = 0x90;
constexpr UINT vkScroll = 0x91;
constexpr UINT vkLShift = 0xA0;
constexpr UINT vkRShift = 0xA1;
constexpr UINT vkLControl = 0xA2;
constexpr UINT vkRControl = 0xA3;
constexpr UINT vkLMenu = 0xA4;
constexpr UINT vkRMenu = 0xA5;
constexpr UINT vkOem3 = 0xC0;
constexpr UINT vkF1 = 0x70;
constexpr UINT vkF12 = 0x7B;
constexpr UINT vkOem1 = 0xBA; // ';'

constexpr int scanCodeShiftL = 0x2A;
constexpr int scanCodeShiftR = 0x36;
constexpr int scanCodeCtrl = 0x1D;

// ------------------------------------------------------ key translation --

void appendLocalEngineLog(const std::wstring& message);


// Decoupled key event payload: the DLL adapts from Ime::KeyEvent, the
// helper from the wire JSON keyEvent object.
struct KeyEventData {
  UINT keyCode = 0;
  UINT charCode = 0;
  UINT scanCode = 0;
  bool isExtended = false;
  const BYTE* keyStates = nullptr; // 256 bytes
};

int translateKeyCode(const KeyEventData& e);
int translateModifiers(const KeyEventData& e, bool isUp);
bool isSecondSelectionShortcut(const KeyEventData& e);
int selectionShortcutConsumeCode(const KeyEventData& e);
bool isBareShiftKeyCode(UINT keyCode);
bool isKeyReleaseRelevantForBackend(UINT keyCode);
// UTF-8 byte position -> code point index (compositionCaretRuneIndex).
int compositionCaretRuneIndex(const std::string& composition, int caretBytePos);

// ---------------------------------------------- 输入状态共享（跨进程） --
// Go rime.go 的 shared input state 移植：ascii_mode / traditionalization /
// ascii_punct / full_shape 四个输入状态选项在应用间共享，落盘
// %APPDATA%\Moqi\appearance_config.json（input_state_shared + shared_options），
// 会话创建时套用、Shift 抬起 / Caps 按下 / 菜单开关后捕获变化回写。
// daemon 会话（32 位）与 DLL 进程内会话（64 位）共用本模块——文件即跨
// 进程通道（Go 同款：激活时重新读取，切换应用即跟随）。
struct SharedInputState {
  bool enabled = false;
  std::map<std::string, bool> options;  // 仅上述四键
};

// 读文件（带 mtime 缓存，热路径零重复 IO）。
SharedInputState loadSharedInputStateCached();
// 读改写：只动 input_state_shared / shared_options 两键，其余键原样保留。
void saveSharedInputState(const SharedInputState& s);

// 会话建立/激活后：把共享值压进 rime 会话（未启用或无值时为空操作）。
void applySharedInputState(rimeapi::SessionId id);
// Shift 抬起 / Caps 按下的捕获触发点（Go shouldSyncSharedInputStateAfterProcessKey
// 同款判定在内部做）。共享开启且有变化时回写文件。
void syncSharedAfterKey(rimeapi::SessionId id, UINT vkCode, bool isUp);
// 菜单/按钮开关了共享四选项后的捕获（不看键型，直接读会话比对）。
void syncSharedAfterOptionToggle(rimeapi::SessionId id);
// 菜单把共享打开：无视文件里的旧 enabled 值，从会话取基线并连同
// enabled=true 落盘（Go toggleInputStateShared 的开启分支）。
void enableSharedFromSession(rimeapi::SessionId id);

// 方案跟随：current_schema_id 的读/写（RMW 只动一键）。daemon 侧切换方案时
// 落盘，进程内引擎在激活时据此 SelectSchema。
std::string loadSyncedSchemaId();
void saveSyncedSchemaId(const std::string& schemaId);

// ---------------------------------------------- 用户词典学习（跨进程） --
// 每进程 userdb 独占（RimeInProc\p-*），学习经 rime 原生 sync 快照汇入主
// 词典。DLL 在失焦/会话空闲时调用：导出本进程快照并拷到主词典 sync/
// 目录，返回拷贝的文件数（0=无学习或导出失败）。内部持 runtime 互斥并
// join 维护线程——只在无组字窗口调用（会话已销毁的 deactivate 时刻）。
// daemon 收到通知后对主词典执行 RimeSyncUserData 完成合并。
int syncInprocUserDbToMaster();

// 反向：把主词典 sync/ 下其它进程的快照灌进本进程用户目录并合并
//（RimeSyncUserData）。用于"另一个应用刚学会的词，本应用也要认识"。
// 会清掉全部会话：调用方须保证无组字并在返回后重建会话。
bool importMasterUserDbSnapshots();

} // namespace Moqi

#endif // MOQI_RIME_CORE_H_
