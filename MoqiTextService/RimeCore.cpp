//
//	RimeCore.cpp: engine core shared by the TSF DLL and moqi-helper.
//	Extracted verbatim from LocalEngine.cpp (phase 5, 2026-11-01); the
//	per-key semantics still mirror the Go backend hot path.
//
#include "RimeCore.h"
#include "LocalRerank.h"

#include <ShlObj.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace std;

namespace Moqi {

// -------------------------------------------------- utf8 <-> utf16 locals --
// 与 libIME2 Utils 同义，但 RimeCore 不依赖 libIME2（Helper 进程也要用）。
wstring utf8ToWide(const char* text) {
  int len = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  if (len <= 1) return wstring();
  wstring out(static_cast<size_t>(len - 1), L'x');
  ::MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], len);
  return out;
}

string wideToUtf8(const wchar_t* text) {
  int len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return string();
  string out(static_cast<size_t>(len - 1), 'x');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], len, nullptr, nullptr);
  return out;
}

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
  const wstring name = utf8ToWide(current.asCString());
  if (name.empty() || name == L"." || name == L".." ||
      name.find_first_of(L"\\/") != wstring::npos) {
    return L"Rime";
  }
  return name;
}

// --------------------------------------------------------- key constants --


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

bool isPrintableChar(const KeyEventData& e) {
  const UINT charCode = e.charCode;
  return charCode > 0x1f && charCode != 0x7f;
}

int shiftSide(const KeyEventData& e) {
  if (e.scanCode == scanCodeShiftR) {
    return vkRShift;
  }
  if (e.scanCode == scanCodeShiftL) {
    return vkLShift;
  }
  const BYTE* states = e.keyStates;
  const bool rShiftDown = (states[vkRShift] & 0x80) != 0;
  const bool lShiftDown = (states[vkLShift] & 0x80) != 0;
  if (rShiftDown && !lShiftDown) {
    return vkRShift;
  }
  return vkLShift;
}

int controlSide(const KeyEventData& e) {
  if (e.scanCode == scanCodeCtrl && e.isExtended) {
    return vkRControl;
  }
  if (e.scanCode == 0) {
    const BYTE* states = e.keyStates;
    const bool rCtrlDown = (states[vkRControl] & 0x80) != 0;
    const bool lCtrlDown = (states[vkLControl] & 0x80) != 0;
    if (rCtrlDown && !lCtrlDown) {
      return vkRControl;
    }
  }
  return vkLControl;
}

// Port of rime_keyevent.go translateKeyCode.
int translateKeyCode(const KeyEventData& e) {
  UINT keyCode = e.keyCode;
  if (keyCode == vkShift) {
    keyCode = static_cast<UINT>(shiftSide(e));
  } else if (keyCode == vkControl) {
    keyCode = static_cast<UINT>(controlSide(e));
  }

  const int mapped = mapVirtualKey(keyCode);
  if (mapped != 0) {
    // OEM_3 is the only printable key in the constant map; Shift+` already
    // carries '~' as charCode, which must win (2026-10-29 backend fix).
    if (keyCode == vkOem3 && isPrintableChar(e) && e.charCode != '`') {
      return static_cast<int>(e.charCode);
    }
    return mapped;
  }
  if (isPrintableChar(e)) {
    return static_cast<int>(e.charCode);
  }
  if (keyCode >= vkF1 && keyCode <= vkF12) {
    return kRimeF1 + static_cast<int>(keyCode) - vkF1;
  }
  return kVoidSymbol;
}

// Port of rime_keyevent.go translateModifiers.
int translateModifiers(const KeyEventData& e, bool isUp) {
  const BYTE* states = e.keyStates;
  const UINT keyCode = e.keyCode;
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

bool isSecondSelectionShortcut(const KeyEventData& e) {
  const BYTE* states = e.keyStates;
  if ((states[vkShift] & 0x80) != 0 || (states[vkControl] & 0x80) != 0 ||
      (states[vkMenu] & 0x80) != 0) {
    return false;
  }
  if (e.keyCode == vkOem1) {
    return true;
  }
  return e.charCode != 0 && e.charCode == kSecondSelectChar;
}

int selectionShortcutConsumeCode(const KeyEventData& e) {
  if (isSecondSelectionShortcut(e)) {
    return kSecondSelectChar;
  }
  if (e.charCode >= '1' && e.charCode <= '9') {
    return static_cast<int>(e.charCode);
  }
  if (e.keyCode >= '1' && e.keyCode <= '9') {
    return static_cast<int>(e.keyCode);
  }
  if (e.keyCode != 0) {
    return static_cast<int>(e.keyCode);
  }
  return static_cast<int>(e.charCode);
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

// RimeCore 日志出口：宿主（DLL/Helper）注入，默认静默。
void (*g_coreLogSink)(const std::wstring&) = nullptr;

void setCoreLogSink(void (*sink)(const std::wstring&)) {
  g_coreLogSink = sink;
}

void coreLog(const std::wstring& message) {
  if (g_coreLogSink != nullptr) {
    g_coreLogSink(message);
  }
}

void appendLocalEngineLog(const wstring& message) {
  coreLog(L"[local-engine] " + message);
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
  // 2026-11-01 生产回滚：多进程并发开同一 userdb 实证触发 LevelDB LOCK
  // （后到进程 schema 加载失败 -> 空引擎吃键 -> 直出拼音）。用户词典共享改走
  // MoqiDaemon 单写者设计（阶段5 奇偶矩阵内），在此之前 RimeInProc 禁用。
  body += "  \"translator/enable_user_dict\": false\n";
  body += "  \"melt_eng/enable_user_dict\": false\n";
  return body;
}

// =========================================================== RimeRuntime ==

RimeRuntime& RimeRuntime::instance() {
  static RimeRuntime runtime;
  return runtime;
}

bool RimeRuntime::supported() {
#if !defined(_WIN64) && !defined(MOQI_ALLOW_X86_ENGINE)
  // x86 TSF DLL has no engine dll to load; the helper defines
  // MOQI_ALLOW_X86_ENGINE because it ships with an x86 rime.dll.
  return false;
#else
  // We only ship an x64 rime.dll; the x86 TSF DLL stays on the RPC path.
  wchar_t value[8] = {};
  const DWORD len =
      ::GetEnvironmentVariableW(L"MOQI_LOCAL_ENGINE", value, _countof(value));
  return !(len > 0 && len < _countof(value) && wcscmp(value, L"0") == 0);
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
    appendLocalEngineLog(L"init thread begin");
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

    const string sharedDirUtf8 = wideToUtf8(sharedDir.c_str());
    const string userDirUtf8 = wideToUtf8(userDir.c_str());
    const string prebuiltUtf8 = wideToUtf8(prebuiltDir.c_str());
    const string stagingUtf8 = wideToUtf8((userDir + L"\\build").c_str());
    const string appDataRootUtf8 = wideToUtf8(appDataRoot.c_str());
    wstring logDirW = localAppDataDir();
    if (!logDirW.empty()) {
      logDirW += L"\\MoqiIM\\Log";
      filesystem::create_directories(logDirW, ec);
    }
    const string logDirUtf8 =
        logDirW.empty() ? string() : wideToUtf8(logDirW.c_str());

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

} // namespace Moqi
