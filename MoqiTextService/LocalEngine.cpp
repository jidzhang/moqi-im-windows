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

// ---------------------------------------------------------------- paths --

wstring appDataRoamingDir() {
  wchar_t* appData = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                       &appData))) {
    wstring dir(appData);
    ::CoTaskMemFree(appData);
    return dir;
  }
  return L"";
}

wstring localAppDataDir() {
  wchar_t* appData = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &appData))) {
    wstring dir(appData);
    ::CoTaskMemFree(appData);
    return dir;
  }
  return L"";
}

// %APPDATA%\Moqi — shared config root used by the Go backend
// (moqiAppDataDir(), scheme_set.go).
wstring moqiAppDataDir() {
  const wstring roaming = appDataRoamingDir();
  return roaming.empty() ? L"" : roaming + L"\\Moqi";
}

// Reads scheme_set.json {"current": "<name>"} with the same defaults as the
// backend (scheme_set.go); falls back to "Rime".
wstring currentSchemeSetDirName() {
  const wstring root = moqiAppDataDir();
  if (root.empty()) {
    return L"Rime";
  }
  ifstream file(root + L"\\scheme_set.json", ifstream::binary);
  if (!file) {
    return L"Rime";
  }
  Json::Value cfg;
  file >> cfg;
  const Json::Value current = cfg.get("current", "Rime");
  if (!current.isString()) {
    return L"Rime";
  }
  const wstring name = utf8ToUtf16(current.asCString());
  if (name.empty() || name == L"." || name == L".." ||
      name.find_first_of(L"\\/") != wstring::npos) {
    return L"Rime";
  }
  return name;
}

// --------------------------------------------------------- key constants --

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

constexpr wchar_t kSecondSelectChar = L';';

int mapVirtualKey(UINT keyCode) {
  switch (keyCode) {
  case vkBack: return kRimeBackSpace;
  case vkTab: return kRimeTab;
  case vkClear: return kRimeClear;
  case vkReturn: return kRimeReturn;
  case vkMenu: return kRimeAltL;
  case vkPause: return kRimePause;
  case vkCapital: return kRimeCapsLock;
  case vkEscape: return kRimeEscape;
  case vkSpace: return 0x020;
  case vkPrior: return kRimePrior;
  case vkNext: return kRimeNext;
  case vkEnd: return kRimeEnd;
  case vkHome: return kRimeHome;
  case vkLeft: return kRimeLeft;
  case vkUp: return kRimeUp;
  case vkRight: return kRimeRight;
  case vkDown: return kRimeDown;
  case vkSelect: return kRimeSelect;
  case vkPrint: return kRimePrint;
  case vkExecute: return kRimeExecute;
  case vkInsert: return kRimeInsert;
  case vkDelete: return kRimeDelete;
  case vkHelp: return kRimeHelp;
  case vkLWin: return kRimeMetaL;
  case vkRWin: return kRimeMetaR;
  case vkNumLock: return kRimeNumLock;
  case vkScroll: return kRimeScrollLock;
  case vkLShift: return kRimeShiftL;
  case vkRShift: return kRimeShiftR;
  case vkLControl: return kRimeControlL;
  case vkRControl: return kRimeControlR;
  case vkLMenu: return kRimeAltL;
  case vkRMenu: return kRimeAltR;
  case vkOem3: return '`';
  default: return 0;
  }
}

bool isPrintableChar(Ime::KeyEvent& e) {
  const UINT charCode = e.charCode();
  return charCode > 0x1f && charCode != 0x7f;
}

int shiftSide(Ime::KeyEvent& e) {
  if (e.scanCode() == scanCodeShiftR) {
    return vkRShift;
  }
  if (e.scanCode() == scanCodeShiftL) {
    return vkLShift;
  }
  const BYTE* states = e.keyStates();
  const bool rShiftDown = (states[vkRShift] & 0x80) != 0;
  const bool lShiftDown = (states[vkLShift] & 0x80) != 0;
  if (rShiftDown && !lShiftDown) {
    return vkRShift;
  }
  return vkLShift;
}

int controlSide(Ime::KeyEvent& e) {
  if (e.scanCode() == scanCodeCtrl && e.isExtended()) {
    return vkRControl;
  }
  if (e.scanCode() == 0) {
    const BYTE* states = e.keyStates();
    const bool rCtrlDown = (states[vkRControl] & 0x80) != 0;
    const bool lCtrlDown = (states[vkLControl] & 0x80) != 0;
    if (rCtrlDown && !lCtrlDown) {
      return vkRControl;
    }
  }
  return vkLControl;
}

// Port of rime_keyevent.go translateKeyCode.
int translateKeyCode(Ime::KeyEvent& e) {
  UINT keyCode = e.keyCode();
  if (keyCode == vkShift) {
    keyCode = static_cast<UINT>(shiftSide(e));
  } else if (keyCode == vkControl) {
    keyCode = static_cast<UINT>(controlSide(e));
  }

  const int mapped = mapVirtualKey(keyCode);
  if (mapped != 0) {
    // OEM_3 is the only printable key in the constant map; Shift+` already
    // carries '~' as charCode, which must win (2026-10-29 backend fix).
    if (keyCode == vkOem3 && isPrintableChar(e) && e.charCode() != '`') {
      return static_cast<int>(e.charCode());
    }
    return mapped;
  }
  if (isPrintableChar(e)) {
    return static_cast<int>(e.charCode());
  }
  if (keyCode >= vkF1 && keyCode <= vkF12) {
    return kRimeF1 + static_cast<int>(keyCode) - vkF1;
  }
  return kVoidSymbol;
}

// Port of rime_keyevent.go translateModifiers.
int translateModifiers(Ime::KeyEvent& e, bool isUp) {
  const BYTE* states = e.keyStates();
  const UINT keyCode = e.keyCode();
  int result = 0;
  if (keyCode != vkShift && (states[vkShift] & 0x80) != 0) {
    result |= kShiftMask;
  }
  if ((states[vkCapital] & 1) != 0) {
    result |= kLockMask;
  }
  if (keyCode != vkControl && (states[vkControl] & 0x80) != 0) {
    result |= kControlMask;
  }
  if (keyCode != vkMenu && (states[vkMenu] & 0x80) != 0) {
    result |= kAltMask;
  }
  if (isUp) {
    result |= kReleaseMask;
  }
  if (keyCode == vkCapital && !isUp) {
    result ^= kLockMask;
  }
  return result;
}

bool isBareShiftKeyCode(UINT keyCode) { return keyCode == vkShift; }

// Only Shift (toggles via release) and CapsLock have meaningful releases for
// the engine; forwarding ordinary key releases disturbs sentence context.
bool isKeyReleaseRelevantForBackend(UINT keyCode) {
  return keyCode == vkShift || keyCode == vkCapital;
}

bool isSecondSelectionShortcut(Ime::KeyEvent& e) {
  const BYTE* states = e.keyStates();
  if ((states[vkShift] & 0x80) != 0 || (states[vkControl] & 0x80) != 0 ||
      (states[vkMenu] & 0x80) != 0) {
    return false;
  }
  if (e.keyCode() == vkOem1) {
    return true;
  }
  return e.charCode() != 0 && e.charCode() == kSecondSelectChar;
}

int selectionShortcutConsumeCode(Ime::KeyEvent& e) {
  if (isSecondSelectionShortcut(e)) {
    return kSecondSelectChar;
  }
  if (e.charCode() >= '1' && e.charCode() <= '9') {
    return static_cast<int>(e.charCode());
  }
  if (e.keyCode() >= '1' && e.keyCode() <= '9') {
    return static_cast<int>(e.keyCode());
  }
  if (e.keyCode() != 0) {
    return static_cast<int>(e.keyCode());
  }
  return static_cast<int>(e.charCode());
}

// Port of compositionCaretRuneIndex: UTF-8 byte position -> code point index.
int compositionCaretRuneIndex(const string& composition, int caretBytePos) {
  if (caretBytePos <= 0 || composition.empty()) {
    return 0;
  }
  const auto countRunes = [&composition](size_t end) {
    int runes = 0;
    for (size_t i = 0; i < end;) {
      unsigned char c = static_cast<unsigned char>(composition[i]);
      size_t step = 1;
      if ((c & 0xE0) == 0xC0) {
        step = 2;
      } else if ((c & 0xF0) == 0xE0) {
        step = 3;
      } else if ((c & 0xF8) == 0xF0) {
        step = 4;
      }
      i += step;
      ++runes;
    }
    return runes;
  };
  const size_t len = composition.size();
  if (caretBytePos >= static_cast<int>(len)) {
    return countRunes(len);
  }
  return countRunes(static_cast<size_t>(caretBytePos));
}

// ------------------------------------------------------------ file utils --

void appendLocalEngineLog(const wstring& message) {
  // Reuse the always-on key guard log: low-frequency lifecycle events only.
  appendRpcGuardLog(L"[local-engine] " + message);
}

string readFileUtf8(const wstring& path) {
  ifstream file(path, ios::binary);
  if (!file) {
    return "";
  }
  ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void writeFileUtf8(const wstring& path, const string& content) {
  ofstream file(path, ios::binary | ios::trunc);
  if (!file) {
    return;
  }
  file.write(content.data(), static_cast<streamsize>(content.size()));
}

// Builds the in-proc rime_frost.custom.yaml: the backend's generated patch
// (engine behavior parity: grammar, typo edges, mixed-input learning rules).
// 阶段3（2026-11-01）：用户词典开启。实测 rime userdb（leveldb 封装）在本
// 构建上锁文件不互斥——多进程并发开同一库可写、数据合并无损（试验台
// udbtest 双进程交叉提交验证，见 24-交接）。进程内学习即时生效，跨进程
// 学习在对方下次启动时可见。
string buildInProcCustomPatch(const wstring& backendUserDir) {
  const string backendPatch =
      readFileUtf8(backendUserDir + L"\\rime_frost.custom.yaml");
  string body = "# managed by moqi local engine\npatch:\n";
  if (!backendPatch.empty()) {
    istringstream stream(backendPatch);
    string line;
    while (getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty() || line[0] == '#' || line.rfind("patch:", 0) == 0) {
        continue;
      }
      body += line;
      body += '\n';
    }
  }
  return body;
}

} // namespace

// =========================================================== RimeRuntime ==

RimeRuntime& RimeRuntime::instance() {
  static RimeRuntime runtime;
  return runtime;
}

bool RimeRuntime::supported() {
#ifdef _WIN64
  // We only ship an x64 rime.dll; the x86 TSF DLL stays on the RPC path.
  wchar_t value[8] = {};
  const DWORD len =
      ::GetEnvironmentVariableW(L"MOQI_LOCAL_ENGINE", value, _countof(value));
  return !(len > 0 && len < _countof(value) && wcscmp(value, L"0") == 0);
#else
  return false;
#endif
}

bool RimeRuntime::loadLibrary(const wstring& rimeDir) {
  const wstring dllPath = rimeDir + L"\\rime.dll";
  const DWORD attrs = ::GetFileAttributesW(dllPath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    appendLocalEngineLog(L"rime.dll not found: " + dllPath +
                         L" attrs=" + to_wstring(attrs));
    return false;
  }
  // LOAD_WITH_ALTERED_SEARCH_PATH resolves rime.dll's dependencies from its
  // own directory instead of the host app's. Retry on the transient errors a
  // freshly deployed DLL can hit (AV on-access scan holding the file).
  for (int attempt = 0;; ++attempt) {
    module_ = ::LoadLibraryExW(dllPath.c_str(), nullptr,
                               LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module_ != nullptr) {
      break;
    }
    const DWORD err = ::GetLastError();
    const bool transient =
        err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED ||
        err == ERROR_LOCK_VIOLATION;
    appendLocalEngineLog(L"LoadLibraryExW failed: " + dllPath + L" err=" +
                         to_wstring(err) + L" attempt=" + to_wstring(attempt));
    if (!transient || attempt >= 4) {
      return false;
    }
    ::Sleep(300);
  }

#define RESOLVE_PROC(name, var)                                                \
  do {                                                                         \
    var = reinterpret_cast<decltype(var)>(                                     \
        (void*)::GetProcAddress(module_, name));                               \
    if (var == nullptr) {                                                      \
      return false;                                                            \
    }                                                                          \
  } while (0)

  RESOLVE_PROC("RimeSetup", rimeSetup);
  RESOLVE_PROC("RimeInitialize", rimeInitialize);
  RESOLVE_PROC("RimeStartMaintenance", rimeStartMaintenance);
  RESOLVE_PROC("RimeJoinMaintenanceThread", rimeJoinMaintenanceThread);
  RESOLVE_PROC("RimeCreateSession", rimeCreateSession);
  RESOLVE_PROC("RimeFindSession", rimeFindSession);
  RESOLVE_PROC("RimeDestroySession", rimeDestroySession);
  RESOLVE_PROC("RimeProcessKey", rimeProcessKey);
  RESOLVE_PROC("RimeClearComposition", rimeClearComposition);
  RESOLVE_PROC("RimeGetCommit", rimeGetCommit);
  RESOLVE_PROC("RimeFreeCommit", rimeFreeCommit);
  RESOLVE_PROC("RimeGetContext", rimeGetContext);
  RESOLVE_PROC("RimeFreeContext", rimeFreeContext);
  RESOLVE_PROC("RimeSetOption", rimeSetOption);
  RESOLVE_PROC("RimeGetOption", rimeGetOption);
  RESOLVE_PROC("RimeSelectCandidateOnCurrentPage",
               rimeSelectCandidateOnCurrentPage);
  RESOLVE_PROC("RimeGetCurrentSchema", rimeGetCurrentSchema);
  // Optional exports: upstream defines these static (api-struct only) in
  // some builds; absence degrades gracefully like Go's procAvailable.
  rimeHighlightCandidateOnCurrentPage =
      reinterpret_cast<decltype(rimeHighlightCandidateOnCurrentPage)>(
          (void*)::GetProcAddress(module_,
                                  "RimeHighlightCandidateOnCurrentPage"));
  rimeChangePage = reinterpret_cast<decltype(rimeChangePage)>(
      (void*)::GetProcAddress(module_, "RimeChangePage"));
  RESOLVE_PROC("RimeSchemaOpen", rimeSchemaOpen);
  RESOLVE_PROC("RimeConfigClose", rimeConfigClose);
  RESOLVE_PROC("RimeConfigSetInt", rimeConfigSetInt);
#undef RESOLVE_PROC

  iconDir_ = rimeDir + L"\\icons";
  return true;
}

void RimeRuntime::requestInit(const wstring& programDir,
                              const wstring& backendDir) {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  if (state_ != State::Idle) {
    return;
  }
  state_ = State::Loading;
  thread([this, programDir, backendDir]() {
    const wstring rimeDir =
        programDir + L"\\" + backendDir + L"\\input_methods\\rime";
    if (!loadLibrary(rimeDir)) {
      std::lock_guard<std::mutex> lock(stateMutex_);
      state_ = State::Failed;
      return;
    }

    const wstring appDataRoot = moqiAppDataDir();
    const wstring backendUserDir =
        appDataRoot.empty() ? L"" : appDataRoot + L"\\" + currentSchemeSetDirName();
    if (appDataRoot.empty() || backendUserDir.empty()) {
      std::lock_guard<std::mutex> lock(stateMutex_);
      state_ = State::Failed;
      return;
    }
    const wstring sharedDir = rimeDir + L"\\data";
    const wstring userDir = appDataRoot + L"\\RimeInProc";
    const wstring prebuiltDir = backendUserDir + L"\\build";

    error_code ec;
    filesystem::create_directories(userDir, ec);
    if (ec) {
      appendLocalEngineLog(L"create user dir failed: " + userDir);
      std::lock_guard<std::mutex> lock(stateMutex_);
      state_ = State::Failed;
      return;
    }

    // Seed the engine-behavior patch before deployment picks it up.
    const string patch = buildInProcCustomPatch(backendUserDir);
    const wstring patchPath = userDir + L"\\rime_frost.custom.yaml";
    if (readFileUtf8(patchPath) != patch) {
      writeFileUtf8(patchPath, patch);
    }

    // 阶段3种子：RimeInProc 首次启用用户词典时，把后端已积累的学习数据
    // 带过来（leveldb 拷贝中的撕裂由打开时的 recovery 自愈）。
    const wstring srcUserDb = backendUserDir + L"\\rime_frost.userdb";
    const wstring dstUserDb = userDir + L"\\rime_frost.userdb";
    if (!filesystem::exists(dstUserDb) && filesystem::exists(srcUserDb)) {
      error_code copyEc;
      filesystem::copy(srcUserDb, dstUserDb,
                       filesystem::copy_options::recursive, copyEc);
    }

    // Cross-process gate: only one app process runs the first deployment
    // into the shared in-proc dir; the others then find it up to date.
    HANDLE initMutex = ::CreateMutexW(nullptr, TRUE, L"MoqiRimeInProcInit");
    bool locked = initMutex != nullptr;
    if (locked && ::GetLastError() == ERROR_ALREADY_EXISTS) {
      locked = ::WaitForSingleObject(initMutex, 30000) == WAIT_OBJECT_0;
    }
    if (!locked) {
      if (initMutex != nullptr) {
        ::CloseHandle(initMutex);
      }
      appendLocalEngineLog(L"init mutex acquisition failed");
      std::lock_guard<std::mutex> lock(stateMutex_);
      state_ = State::Failed;
      return;
    }

    const string sharedDirUtf8 = utf16ToUtf8(sharedDir.c_str());
    const string userDirUtf8 = utf16ToUtf8(userDir.c_str());
    const string prebuiltUtf8 = utf16ToUtf8(prebuiltDir.c_str());
    const string stagingUtf8 = utf16ToUtf8((userDir + L"\\build").c_str());
    const string appDataRootUtf8 = utf16ToUtf8(appDataRoot.c_str());
    wstring logDirW = localAppDataDir();
    if (!logDirW.empty()) {
      logDirW += L"\\MoqiIM\\Log";
      filesystem::create_directories(logDirW, ec);
    }
    const string logDirUtf8 =
        logDirW.empty() ? string() : utf16ToUtf8(logDirW.c_str());

    {
      std::lock_guard<std::mutex> apiLock(apiMutex_);
      rimeapi::Traits traits = {};
      traits.data_size = static_cast<int>(sizeof(rimeapi::Traits) -
                                          sizeof(traits.data_size));
      traits.shared_data_dir = sharedDirUtf8.c_str();
      traits.user_data_dir = userDirUtf8.c_str();
      traits.distribution_name = "Rime";
      traits.distribution_code_name = "Moqi";
      traits.distribution_version = "inproc";
      traits.app_name = "Rime.Moqi";
      traits.min_log_level = 2; // error, matches product default
      traits.log_dir = logDirUtf8.c_str();
      traits.prebuilt_data_dir = prebuiltUtf8.c_str();
      traits.staging_dir = stagingUtf8.c_str();
      rimeSetup(&traits);
      rimeInitialize(&traits);
      // A false return also means "no changes detected" (nothing to
      // deploy); join is a no-op when maintenance never started.
      rimeStartMaintenance(FALSE);
      rimeJoinMaintenanceThread();
    }

    sharedDataDirUtf8_ = sharedDirUtf8;
    appDataRootUtf8_ = appDataRootUtf8;
    // 阶段2重排数据在后台线程预载（bigram TSV 3.4MB），不占首键时间。
    RerankData::instance().ensureLoaded(sharedDirUtf8, appDataRootUtf8);
    TypoMemoryStore::instance().ensureLoaded(appDataRootUtf8);

    ::ReleaseMutex(initMutex);
    ::CloseHandle(initMutex);

    std::lock_guard<std::mutex> lock(stateMutex_);
    state_ = State::Ready;
    appendLocalEngineLog(L"engine ready, user dir=" + userDir);
  }).detach();
}

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
  if (digit || (semicolonSelectSecond_ && isSecondSelectionShortcut(e))) {
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

  const int keycode0 = translateKeyCode(e);
  const int mask = translateModifiers(e, isUp);
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
  if (!semicolonSelectSecond_ || !isSecondSelectionShortcut(e)) {
    return false;
  }
  bool engineOk = false;
  const EngineState state = currentState(&engineOk);
  if (!engineOk || state.composition.empty() || state.candidates.size() < 2) {
    return false;
  }
  secondSelectConsumeKeyUpCode_ = selectionShortcutConsumeCode(e);
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::handleSecondSelectionDown(Ime::KeyEvent& e,
                                            Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(e) != secondSelectConsumeKeyUpCode_) {
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
      selectionShortcutConsumeCode(e) != secondSelectConsumeKeyUpCode_) {
    return false;
  }
  resp["returnValue"] = 1;
  return true;
}

bool LocalEngine::handleSecondSelectionUp(Ime::KeyEvent& e,
                                          Json::Value& resp) {
  if (secondSelectConsumeKeyUpCode_ == 0 ||
      selectionShortcutConsumeCode(e) != secondSelectConsumeKeyUpCode_) {
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
