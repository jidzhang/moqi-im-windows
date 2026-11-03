//
//	RimeCore.cpp: engine core shared by the TSF DLL and moqi-helper.
//	Extracted verbatim from LocalEngine.cpp (phase 5, 2026-11-01); the
//	per-key semantics still mirror the Go backend hot path.
//
#include "RimeCore.h"
#include "LocalRerank.h"

#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#pragma comment(lib, "ole32.lib")  // CoCreateGuid（每进程用户目录名）

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
  {
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, file, &cfg, &errs)) {
      return L"Rime";
    }
  }
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
namespace {
bool jsonBool(const Json::Value& v, const char* key) {
  const Json::Value& x = v[key];
  return x.isBool() && x.asBool();
}
string jsonString(const Json::Value& v, const char* key) {
  const Json::Value& x = v[key];
  return x.isString() ? x.asString() : string();
}

// Go grammar_model.go：auto 扫描 shared/user 目录，取 >=50MB 的最大 .gram。
string detectAutoGrammar(const vector<wstring>& dirs) {
  string best;
  uintmax_t bestSize = 0;
  const uintmax_t kMin = 50ull << 20;
  for (const auto& dir : dirs) {
    if (dir.empty()) continue;
    error_code ec;
    for (const auto& de : filesystem::directory_iterator(dir, ec)) {
      if (ec) break;
      if (!de.is_regular_file()) continue;
      const wstring name = de.path().filename().wstring();
      if (name.size() < 5) continue;
      wstring lower = name;
      for (wchar_t& c : lower) c = (wchar_t)::towlower(c);
      if (lower.size() < 5 || lower.substr(lower.size() - 5) != L".gram") {
        continue;
      }
      const uintmax_t size = de.file_size(ec);
      if (ec || size < kMin || size <= bestSize) continue;
      bestSize = size;
      best = wideToUtf8(name.substr(0, name.size() - 5).c_str());
    }
  }
  return best;
}

// Builds the product-behavior patch that Go used to generate from
// local_behavior.json (grammar, typo edges, mixed-input learning rules).
string buildLocalBehaviorPatch(const wstring& sharedDataDir) {
  Json::Value cfg;
  const wstring path = moqiAppDataDir() + L"\\local_behavior.json";
  {
    ifstream in(path, ios::binary);
    if (in) {
      Json::CharReaderBuilder builder;
      string errs;
      if (!Json::parseFromStream(builder, in, &cfg, &errs)) {
        cfg = Json::Value();
      }
    }
  }
  string body = "# managed by moqi local engine\npatch:\n";
  int lines = 0;
  auto emit = [&](const string& line) {
    body += "  " + line + "\n";
    ++lines;
  };
  if (jsonBool(cfg, "disable_slash_punct")) {
    emit("recognizer/patterns/punct: null");
  } else if (jsonBool(cfg, "slash_punct_v_only")) {
    emit("recognizer/patterns/punct: \"^v([0-9]|10|[A-Za-z]+)$\"");
  }
  if (jsonBool(cfg, "disable_unicode_trigger")) {
    emit("recognizer/patterns/unicode: null");
  }
  if (jsonBool(cfg, "disable_number_trigger")) {
    emit("recognizer/patterns/number: null");
  }
  if (jsonBool(cfg, "disable_lunar_trigger")) {
    emit("recognizer/patterns/gregorian_to_lunar: null");
  }
  if (jsonBool(cfg, "disable_calculator")) {
    emit("recognizer/patterns/calculator: null");
  }
  if (jsonBool(cfg, "disable_aux_lookup")) {
    emit("engine/translators/-: table_translator@frost_aux");
    emit("engine/filters/-: lua_filter@*aux_lookup_filter");
  }
  // MixedPair 默认开（Go defaultBigramRerankConfig）；关闭时不再禁用学习。
  bool rerankMixed = true;
  {
    Json::Value contextual;
    ifstream in(moqiAppDataDir() + L"\\contextual_config.json", ios::binary);
    if (in) {
      Json::CharReaderBuilder builder;
      string errs;
      if (Json::parseFromStream(builder, in, &contextual, &errs)) {
        const Json::Value& br = contextual["bigram_rerank"];
        if (br.isObject() && br["mixed_pair"].isBool()) {
          rerankMixed = br["mixed_pair"].asBool();
        }
      }
    }
  }
  if (rerankMixed) {
    emit("translator/disable_user_dict_for_patterns: "
         "[\".*[bcdfhjklmpqstvwxyz]$\"]");
  }

  string setting = jsonString(cfg, "grammar_model");
  string lang;
  if (setting.empty() || setting == "auto" || setting == "AUTO") {
    vector<wstring> dirs;
    if (!sharedDataDir.empty()) dirs.push_back(sharedDataDir);
    dirs.push_back(moqiAppDataDir());
    lang = detectAutoGrammar(dirs);
  } else {
    string lower = setting;
    for (char& c : lower) c = (char)::tolower((unsigned char)c);
    if (lower != "off" && lower != "none" && lower != "false") {
      if (lower.size() > 5 && lower.substr(lower.size() - 5) == ".gram") {
        setting = setting.substr(0, setting.size() - 5);
      }
      lang = setting;
    }
  }
  if (!lang.empty()) {
    emit("grammar/language: " + lang);
    emit("translator/contextual_suggestions: true");
    // 手填罚分优先；否则按出厂模型给 -4（wanxiang 本地模型为 0）。
    double penalty = -4;
    const Json::Value& pv = cfg["grammar_penalty"];
    if (pv.isNumeric()) {
      penalty = pv.asDouble();
      if (penalty < -20 || penalty > 0) penalty = -4;
    } else if (lang.find("wanxiang") != string::npos) {
      penalty = 0;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "grammar/non_collocation_penalty: %g", penalty);
    emit(buf);
    emit("melt_eng/initial_quality: 0.01");
    emit("reduce_english_filter/mode: all");
    emit("translator/sentence_quality_boost: -2");
    emit("translator/enable_transpose: true");
    emit("translator/transpose_credibility: -8");
    emit("translator/enable_deletion: true");
    emit("translator/deletion_credibility: -8");
    emit("translator/enable_substitution: true");
    emit("translator/substitution_credibility: -8");
    emit("translator/enable_insertion: true");
    emit("translator/insertion_credibility: -8");
    emit("translator/typo_second_pass: true");
    emit("translator/typo_second_pass_max_input_length: 12");
    emit("translator/typo_second_pass_insert_after: 5");
  }
  if (lines == 0) return "";
  return body;
}
}  // namespace

string buildInProcCustomPatch(const wstring& backendUserDir,
                              const wstring& sharedDataDir) {
  const string backendPatch =
      readFileUtf8(backendUserDir + L"\\rime_frost.custom.yaml");
  string body = buildLocalBehaviorPatch(sharedDataDir);
  if (body.empty()) body = "# managed by moqi local engine\npatch:\n";
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
  // 2026-11-01 曾因多进程共享一个 RimeInProc 开 userdb 触发 LevelDB LOCK
  // 回滚禁用；2026-11-02 起改为**每进程独立子目录**（p-<hex>），userdb 重新
  // 打开——学习经 rime 原生 sync 快照汇入主词典（见 requestInit 尾部与
  // exportUserDbSnapshot）。
  return body;
}

// ------------------------------------------ 置顶短语 / 超级简拼（Go 等价）--
namespace {
struct CustomPhraseEntry {
  std::string text;
  std::string code;
  int weight = 0;
  int order = 0;
};
struct CustomPhraseCache {
  std::mutex mutex;
  bool loaded = false;
  std::filesystem::file_time_type mtime{};
  uintmax_t size = 0;
  std::vector<CustomPhraseEntry> entries;
};
struct SuperAbbrevCache {
  std::mutex mutex;
  bool loaded = false;
  std::filesystem::file_time_type mtime{};
  uintmax_t size = 0;
  std::map<std::string, std::string> entries;
};

std::string trimAscii(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (unsigned char)s[b] <= ' ') ++b;
  while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
  return s.substr(b, e - b);
}
std::string lowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return s;
}
bool fileStat(const std::wstring& path, std::filesystem::file_time_type* mt,
              uintmax_t* size) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) return false;
  *mt = std::filesystem::last_write_time(path, ec);
  if (ec) return false;
  *size = std::filesystem::file_size(path, ec);
  return !ec;
}

std::vector<CustomPhraseEntry> loadCustomPhrases() {
  static CustomPhraseCache cache;
  std::lock_guard<std::mutex> lock(cache.mutex);
  const std::wstring path = moqiAppDataDir() + L"\\moqi_custom_phrase.txt";
  std::filesystem::file_time_type mt{};
  uintmax_t size = 0;
  if (!fileStat(path, &mt, &size)) {
    cache.entries.clear();
    cache.loaded = true;
    return {};
  }
  if (cache.loaded && cache.mtime == mt && cache.size == size) {
    return cache.entries;
  }
  std::vector<CustomPhraseEntry> out;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  int order = 0;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trimAscii(line);
    if (t.empty() || t[0] == '#') continue;
    const size_t t1 = t.find('\t');
    if (t1 == std::string::npos) continue;
    const size_t t2 = t.find('\t', t1 + 1);
    const std::string text = trimAscii(t.substr(0, t1));
    const std::string code = trimAscii(
        t.substr(t1 + 1, t2 == std::string::npos ? std::string::npos
                                                : t2 - t1 - 1));
    if (text.empty() || code.empty()) continue;
    int weight = 0;
    if (t2 != std::string::npos) {
      weight = std::atoi(trimAscii(t.substr(t2 + 1)).c_str());
    }
    out.push_back({text, code, weight, order++});
  }
  cache.entries = out;
  cache.mtime = mt;
  cache.size = size;
  cache.loaded = true;
  return out;
}

std::map<std::string, std::string> loadSuperAbbrev() {
  static SuperAbbrevCache cache;
  std::lock_guard<std::mutex> lock(cache.mutex);
  const std::wstring path = moqiAppDataDir() + L"\\moqi_super_abbrev.txt";
  std::filesystem::file_time_type mt{};
  uintmax_t size = 0;
  if (!fileStat(path, &mt, &size)) {
    cache.entries.clear();
    cache.loaded = true;
    return {};
  }
  if (cache.loaded && cache.mtime == mt && cache.size == size) {
    return cache.entries;
  }
  std::map<std::string, std::string> out;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string t = trimAscii(line);
    if (t.empty() || t[0] == '#') continue;
    size_t sep = t.find('\t');
    if (sep == std::string::npos) sep = t.find(' ');
    if (sep == std::string::npos) continue;
    const std::string text = trimAscii(t.substr(0, sep));
    const std::string code = lowerAscii(trimAscii(t.substr(sep + 1)));
    if (text.empty() || code.empty()) continue;
    out[code] = text;
  }
  cache.entries = out;
  cache.mtime = mt;
  cache.size = size;
  cache.loaded = true;
  return out;
}

bool isLowerAscii(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    if (c < 'a' || c > 'z') return false;
  }
  return true;
}
}  // namespace

InputOverlay computeInputOverlay(const std::string& rawInput,
                                 const std::string& composition, int pageNo,
                                 int limit) {
  InputOverlay out;
  if (pageNo > 0 || limit <= 0) return out;
  std::string match = trimAscii(rawInput);
  if (match.empty()) match = trimAscii(composition);
  if (match.empty() || match.find('`') != std::string::npos) return out;
  const std::string lower = lowerAscii(match);

  std::vector<CustomPhraseEntry> matches;
  for (const auto& e : loadCustomPhrases()) {
    const std::string code = lowerAscii(trimAscii(e.code));
    if (code.empty() || code.compare(0, lower.size(), lower) != 0) continue;
    matches.push_back(e);
  }
  std::stable_sort(matches.begin(), matches.end(),
                   [&lower](const CustomPhraseEntry& a,
                            const CustomPhraseEntry& b) {
                     const bool ae = lowerAscii(a.code) == lower;
                     const bool be = lowerAscii(b.code) == lower;
                     if (ae != be) return ae;
                     if (a.weight != b.weight) return a.weight > b.weight;
                     if (a.code.size() != b.code.size())
                       return a.code.size() < b.code.size();
                     return a.order < b.order;
                   });
  std::set<std::string> seen;
  for (const auto& e : matches) {
    if (!seen.insert(e.text).second) continue;
    out.custom.push_back({e.text, ""});
    if ((int)out.custom.size() >= limit) break;
  }

  if (lower.size() <= 3 && isLowerAscii(lower)) {
    const auto entries = loadSuperAbbrev();
    const auto it = entries.find(lower);
    if (it != entries.end()) out.superText = it->second;
  }
  return out;
}

// ------------------------------------------------- 日志级别/清理（对齐 Go）

// Go debug_logging.go：产品默认只记 WARNING(1)；调试档（MoqiLauncher.json
// logLevel=debug/trace，两种拼写都认）放行 INFO(0)。INFO 不设限会到 GB 级
//（2026-10-18 实测 ~16GB 事故），别改回 0。
int rimeMinLogLevelForTraits() {
  const wstring dir = localAppDataDir();
  if (dir.empty()) {
    return 1;
  }
  Json::Value cfg;
  ifstream file(dir + L"\\MoqiIM\\MoqiLauncher.json", ifstream::binary);
  if (file) {
    string errs;
    Json::CharReaderBuilder builder;
    Json::parseFromStream(builder, file, &cfg, &errs);
    for (const char* key : {"logLevel", "loglevel"}) {
      string level = cfg.get(key, "").asString();
      transform(level.begin(), level.end(), level.begin(),
                [](unsigned char c) { return (char)tolower(c); });
      if (level == "trace" || level == "debug") {
        return 0;
      }
    }
  }
  return 1;
}

// Go cleanupOversizedLogDir：日志目录超 512MB 时按 mtime 从最旧删起，
// 删除失败（被占用）即停。兜任何"日志点失控"的意外。
void cleanupOversizedLogDir(const wstring& dir) {
  if (dir.empty()) {
    return;
  }
  struct Entry {
    ULARGE_INTEGER written{};
    unsigned long long size = 0;
    wstring path;
  };
  vector<Entry> files;
  unsigned long long total = 0;
  WIN32_FIND_DATAW fd = {};
  HANDLE hFind = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }
    Entry e;
    e.written.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    e.written.HighPart = fd.ftLastWriteTime.dwHighDateTime;
    e.size =
        (unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
    e.path = dir + L"\\" + fd.cFileName;
    total += e.size;
    files.push_back(std::move(e));
  } while (::FindNextFileW(hFind, &fd));
  ::FindClose(hFind);

  const unsigned long long limit = 512ULL << 20;
  if (total <= limit) {
    return;
  }
  sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
    return a.written.QuadPart < b.written.QuadPart;
  });
  for (const auto& e : files) {
    if (total <= limit) {
      break;
    }
    if (!::DeleteFileW(e.path.c_str())) {
      break; // 被占用等：停止，不影响启动
    }
    total -= e.size;
  }
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
  RESOLVE_PROC("RimeSyncUserData", rimeSyncUserData);
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
  RESOLVE_PROC("RimeGetSchemaList", rimeGetSchemaList);
  RESOLVE_PROC("RimeFreeSchemaList", rimeFreeSchemaList);
  RESOLVE_PROC("RimeSelectSchema", rimeSelectSchema);
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
                              const wstring& backendDir,
                              const wstring& dllDir,
                              const wstring& masterUserDir) {
  std::lock_guard<std::mutex> stateLock(stateMutex_);
  if (state_ == State::Failed) {
    // 失败可重试：一次性失败会让进程内引擎永久直落英文（2026-11-03
    // 装机实报：首进程等 init 锁超时后整个应用打不了汉字，重启才好）。
    state_ = State::Idle;
  }
  if (state_ != State::Idle) {
    return;
  }
  state_ = State::Loading;
  thread([this, programDir, backendDir, dllDir, masterUserDir]() {
    appendLocalEngineLog(L"init thread begin");
    const wstring rimeDir =
        programDir + L"\\" + backendDir + L"\\input_methods\\rime";
    if (!loadLibrary(dllDir.empty() ? rimeDir : dllDir)) {
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
    // 用户目录两种模式（2026-11-02 学习闭环）：
    //  masterUserDir 非空（MoqiDaemon）：独占主词典目录，userdb 开启，
    //    DLL 各进程的学习快照汇入这里（LevelDB 锁独占，Go 子进程仍占
    //    Moqi\<方案集> 不冲突）；
    //  空（TSF DLL）：每进程 p-<hex> 子目录，userdb 独占无锁冲突。
    wstring userDir;
    // 每进程独立用户目录（p-<hex>）：userdb 独占无 LevelDB LOCK 冲突；
    // 学习经 rime 原生 sync 快照汇入主词典（exportUserDbSnapshot）。
    if (!masterUserDir.empty()) {
      userDir = masterUserDir;
    } else {
      static const wstring perProcessSubdir = []() {
        GUID g = {};
        wchar_t buf[64] = {};
        if (SUCCEEDED(::CoCreateGuid(&g))) {
          swprintf_s(buf, L"p-%08x%08x", g.Data1,
                     static_cast<unsigned>(g.Data2) << 16 | g.Data3);
        } else {
          swprintf_s(buf, L"p-%08x", ::GetCurrentProcessId());
        }
        return wstring(buf);
      }();
      const wstring inprocRoot = appDataRoot + L"\\RimeInProc";
      userDir = inprocRoot + L"\\" + perProcessSubdir;
      // 孤儿清理：进程崩溃遗留的 p- 目录按最后写入时间超过 7 天删除（尽力
      // 而为；活跃进程的目录被 LevelDB 占用，删除天然失败自保护）。
      WIN32_FIND_DATAW fd = {};
      HANDLE hFind = ::FindFirstFileW((inprocRoot + L"\\p-*").c_str(), &fd);
      if (hFind != INVALID_HANDLE_VALUE) {
        do {
          if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
          ULARGE_INTEGER written{}, nowFile{};
          written.LowPart = fd.ftLastWriteTime.dwLowDateTime;
          written.HighPart = fd.ftLastWriteTime.dwHighDateTime;
          ::GetSystemTimeAsFileTime(reinterpret_cast<FILETIME*>(&nowFile));
          const ULONGLONG ageSec =
              nowFile.QuadPart > written.QuadPart
                  ? (nowFile.QuadPart - written.QuadPart) / 10000000ULL
                  : 0;
          if (ageSec > 7ULL * 24 * 3600) {
            error_code removeEc;
            filesystem::remove_all(inprocRoot + L"\\" + fd.cFileName, removeEc);
          }
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
      }
    }
    // prebuilt 数据目录：优先**安装目录**的 data\build（打包期 rime-prebuild
    // 编译好的 .bin，Go 的 librime.go 用的就是 datadir\build）。此前误指向
    // 用户方案集目录的 build——全新用户没有它，每个新进程都要现场全量编译
    // 词典（几十秒），且与刚启动 daemon 的编译互抢跨进程 init 锁，首个应用
    // 的引擎初始化直接失败、全键直落英文（2026-11-03 用户装机实报）。
    wstring prebuiltDir = sharedDir + L"\\build";
    if (!filesystem::exists(prebuiltDir)) {
      prebuiltDir = backendUserDir + L"\\build"; // 老安装/开发布局兜底
    }

    error_code ec;
    filesystem::create_directories(userDir, ec);
    if (ec) {
      appendLocalEngineLog(L"create user dir failed: " + userDir);
      std::lock_guard<std::mutex> lock(stateMutex_);
      state_ = State::Failed;
      return;
    }

    // Seed the engine-behavior patch before deployment picks it up.
    const string patch = buildInProcCustomPatch(backendUserDir, sharedDir);
    const wstring patchPath = userDir + L"\\rime_frost.custom.yaml";
    if (readFileUtf8(patchPath) != patch) {
      writeFileUtf8(patchPath, patch);
    }

    // 学习种子（2026-11-03 修正）：**不再直接拷主词典的 leveldb 目录**——
    // 主词典正被 daemon 打开并写入，热拷贝得到的是撕裂快照（实测新进程
    // "Error opening db read-only / failed synchronizing"，学到的词全部丢失）。
    // 正确做法：本进程从空 userdb 起步，首次接管时 importMasterUserDbSnapshots
    // 用 rime 原生 sync 的**文本快照**合并学习（文本快照是完整导出，安全）。
    // 仅保留 1.x 时代方案集目录的一次性迁移（那个 db 没有进程在写）。
    const wstring legacyUserDb = backendUserDir + L"\\rime_frost.userdb";
    const wstring dstUserDb = userDir + L"\\rime_frost.userdb";
    if (!filesystem::exists(dstUserDb) && filesystem::exists(legacyUserDb)) {
      error_code copyEc;
      filesystem::copy(legacyUserDb, dstUserDb,
                       filesystem::copy_options::recursive, copyEc);
    }
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      inprocUserDir_ = userDir;
    }

    // Cross-process gate: only one app process runs the first deployment
    // into the shared in-proc dir; the others then find it up to date.
    HANDLE initMutex = ::CreateMutexW(nullptr, TRUE, L"MoqiRimeInProcInit");
    bool locked = initMutex != nullptr;
    if (locked && ::GetLastError() == ERROR_ALREADY_EXISTS) {
      // 前一个持有者可能崩溃/被强杀：此时 WaitForSingleObject 返回
      // WAIT_ABANDONED（不是 WAIT_OBJECT_0）。旧代码只认后者，强杀
      // daemon 后新实例会永久 Failed（2026-11-03 实测日志
      // "init mutex acquisition failed"，引擎不初始化）。两种都算拿到锁。
      const DWORD wait = ::WaitForSingleObject(initMutex, 30000);
      locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
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
      cleanupOversizedLogDir(logDirW);
    }
    const string logDirUtf8 =
        logDirW.empty() ? string() : wideToUtf8(logDirW.c_str());

    {
      std::lock_guard<std::recursive_mutex> apiLock(apiMutex_);
      rimeapi::Traits traits = {};
      traits.data_size = static_cast<int>(sizeof(rimeapi::Traits) -
                                          sizeof(traits.data_size));
      traits.shared_data_dir = sharedDirUtf8.c_str();
      traits.user_data_dir = userDirUtf8.c_str();
      traits.distribution_name = "Rime";
      traits.distribution_code_name = "Moqi";
      traits.distribution_version = "inproc";
      traits.app_name = "Rime.Moqi";
      // 与 Go 对齐（debug_logging.go rimeMinLogLevel）：默认 WARNING(1)，
      // 调试档（MoqiLauncher.json logLevel=debug/trace）放行 INFO(0)。
      // 此前误写 2（ERROR）——审计 2026-11-03。
      traits.min_log_level = rimeMinLogLevelForTraits();
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

// ---------------------------------------------- 首键等待引擎就绪（2026-11-03）
// 全新用户目录首次部署需要 1.5-5 秒（预编译数据 + 用户目录初始部署）。若这段
// 时间里的按键直接放行，用户就会看到"刚装完/刚重启，第一句拼音直落英文"。
// TSF 线程允许短时停顿（既有首键等 RPC 的上限是 10 秒），这里等地等，
// 失败/超时才放行。
bool RimeRuntime::waitUntilReady(int timeoutMs) {
  const ULONGLONG deadline = ::GetTickCount64() + (ULONGLONG)timeoutMs;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(stateMutex_);
      if (state_ == State::Ready) return true;
      if (state_ == State::Failed) return false;
    }
    if (::GetTickCount64() >= deadline) return false;
    ::Sleep(10);
  }
}

// ---------------------------------------------- 输入状态共享（跨进程） --
// 语义与键名对齐 Go appearance_config.go / rime.go：四键 + 文件中转。
// 多进程读改写同一 JSON 存在最后写者赢的竞态——与 Go 多 IME 实例时代
// 相当，且触发频率低（显式开关/Shift/Caps），可接受。

namespace {

const char* kSharedOptionNames[] = {
    "ascii_mode", "traditionalization", "ascii_punct", "full_shape"};

std::wstring sharedAppearanceConfigPath() {
  const wstring root = moqiAppDataDir();
  return root.empty() ? L"" : root + L"\\appearance_config.json";
}

// mtime 缓存：Shift 抬起这类高频点只付一次 stat 的代价。
SharedInputState g_sharedInputStateCache;
FILETIME g_sharedInputStateFileTime = {};
uint64_t g_sharedInputStateCacheTick = 0;
std::mutex g_sharedInputStateMutex;

bool statFileTime(const std::wstring& path, FILETIME* ft) {
  WIN32_FILE_ATTRIBUTE_DATA fad = {};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) ==
      0) {
    return false;
  }
  *ft = fad.ftLastWriteTime;
  return true;
}

Json::Value readJsonRoot(const std::wstring& path) {
  Json::Value v;
  std::ifstream in(path, std::ios::binary);
  if (!in) return v;
  std::string errs;
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  if (!Json::parseFromStream(builder, in, &v, &errs)) {
    return Json::Value();
  }
  return v;
}

bool writeJsonRoot(const std::wstring& path, const Json::Value& root) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  Json::StreamWriterBuilder writer;
  writer["commentStyle"] = "None";
  writer["indentation"] = "  ";
  out << Json::writeString(writer, root);
  return out.good();
}

void readSharedFromJson(const Json::Value& cfg, SharedInputState* out) {
  out->enabled = cfg.get("input_state_shared", false).asBool();
  out->options.clear();
  const Json::Value& opts = cfg["shared_options"];
  if (opts.isObject()) {
    for (const char* name : kSharedOptionNames) {
      if (opts.isMember(name)) {
        out->options[name] = opts[name].asBool();
      }
    }
  }
}

} // namespace

SharedInputState loadSharedInputStateCached() {
  std::lock_guard<std::mutex> lock(g_sharedInputStateMutex);
  const std::wstring path = sharedAppearanceConfigPath();
  if (path.empty()) {
    return g_sharedInputStateCache;
  }
  FILETIME now = {};
  if (statFileTime(path, &now) &&
      (now.dwLowDateTime != g_sharedInputStateFileTime.dwLowDateTime ||
       now.dwHighDateTime != g_sharedInputStateFileTime.dwHighDateTime ||
       g_sharedInputStateCacheTick == 0)) {
    SharedInputState loaded;
    readSharedFromJson(readJsonRoot(path), &loaded);
    g_sharedInputStateCache = loaded;
    g_sharedInputStateFileTime = now;
    g_sharedInputStateCacheTick = ::GetTickCount64();
  }
  return g_sharedInputStateCache;
}

void saveSharedInputState(const SharedInputState& s) {
  std::lock_guard<std::mutex> lock(g_sharedInputStateMutex);
  const std::wstring path = sharedAppearanceConfigPath();
  if (path.empty()) return;
  Json::Value cfg = readJsonRoot(path);
  if (!cfg.isObject()) cfg = Json::Value(Json::objectValue);
  cfg["input_state_shared"] = s.enabled;
  if (s.options.empty()) {
    cfg.removeMember("shared_options");
  } else {
    Json::Value opts(Json::objectValue);
    for (const auto& kv : s.options) opts[kv.first] = kv.second;
    cfg["shared_options"] = opts;
  }
  if (!writeJsonRoot(path, cfg)) return;
  FILETIME now = {};
  if (statFileTime(path, &now)) g_sharedInputStateFileTime = now;
  g_sharedInputStateCache = s;
  g_sharedInputStateCacheTick = ::GetTickCount64();
}

void applySharedInputState(rimeapi::SessionId id) {
  const SharedInputState s = loadSharedInputStateCached();
  if (!s.enabled || s.options.empty() || id == 0) return;
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready()) return;
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  if (runtime.rimeFindSession(id) == 0) return;
  for (const auto& kv : s.options) {
    runtime.rimeSetOption(id, kv.first.c_str(), kv.second ? 1 : 0);
  }
}

// 读会话四键 → 与缓存比对；变化才回写（含文件），返回 true。
bool captureSharedFromSessionLocked(rimeapi::SessionId id,
                                    SharedInputState* s) {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready()) return false;
  std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
  if (runtime.rimeFindSession(id) == 0) return false;
  bool changed = false;
  for (const char* name : kSharedOptionNames) {
    const bool value = runtime.rimeGetOption(id, name) != 0;
    auto it = s->options.find(name);
    if (it == s->options.end() || it->second != value) {
      changed = true;
    }
    s->options[name] = value;
  }
  return changed;
}

void syncSharedAfterKey(rimeapi::SessionId id, UINT vkCode, bool isUp) {
  // Go shouldSyncSharedInputStateAfterProcessKey：Shift 只在抬起、Caps 只在
  // 按下时捕获。
  const bool trigger =
      (vkCode == vkShift && isUp) || (vkCode == vkCapital && !isUp);
  if (!trigger || id == 0) return;
  SharedInputState s = loadSharedInputStateCached();
  if (!s.enabled) return;
  if (captureSharedFromSessionLocked(id, &s)) {
    saveSharedInputState(s);
    appendLocalEngineLog(L"[shared] shift/caps state captured -> file");
  }
}

void syncSharedAfterOptionToggle(rimeapi::SessionId id) {
  SharedInputState s = loadSharedInputStateCached();
  if (!s.enabled || id == 0) return;
  if (captureSharedFromSessionLocked(id, &s)) {
    saveSharedInputState(s);
    appendLocalEngineLog(L"[shared] option toggle captured -> file");
  }
}

void enableSharedFromSession(rimeapi::SessionId id) {
  if (id == 0) return;
  SharedInputState s = loadSharedInputStateCached();
  captureSharedFromSessionLocked(id, &s);
  s.enabled = true;
  saveSharedInputState(s);
  appendLocalEngineLog(L"[shared] enabled, baseline captured -> file");
}

std::string loadSyncedSchemaId() {
  const std::wstring path = sharedAppearanceConfigPath();
  if (path.empty()) return "";
  Json::Value cfg = readJsonRoot(path);
  return cfg.get("current_schema_id", "").asString();
}

void saveSyncedSchemaId(const std::string& schemaId) {
  const std::wstring path = sharedAppearanceConfigPath();
  if (path.empty()) return;
  Json::Value cfg = readJsonRoot(path);
  if (!cfg.isObject()) cfg = Json::Value(Json::objectValue);
  if (schemaId.empty()) {
    cfg.removeMember("current_schema_id");
  } else {
    cfg["current_schema_id"] = schemaId;
  }
  writeJsonRoot(path, cfg);
}

// ---------------------------------------------- 用户词典学习（跨进程） --

int syncInprocUserDbToMaster() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready() || runtime.rimeSyncUserData == nullptr ||
      runtime.rimeJoinMaintenanceThread == nullptr) {
    return 0;
  }
  const std::wstring userDir = runtime.inprocUserDir();
  if (userDir.empty()) return 0;
  {
    std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
    if (runtime.rimeSyncUserData() == 0) {
      appendLocalEngineLog(L"[userdb] RimeSyncUserData failed");
      return 0;
    }
    runtime.rimeJoinMaintenanceThread();
  }
  // 导出快照：<inproc>\sync\<installation-uuid>\*.userdb.txt。全量拷到主词典
  // （Moqi\UserDictMaster，daemon 独占）sync\<p-名>\（重拷幂等，master 合并
  // 也是幂等的）。
  const wstring root = moqiAppDataDir();
  if (root.empty()) return 0;
  const size_t slash = userDir.find_last_of(L'\\');
  const wstring procName =
      slash == wstring::npos ? L"p-unknown" : userDir.substr(slash + 1);
  const wstring dstDir = root + L"\\UserDictMaster\\sync\\" + procName;
  const wstring srcSync = userDir + L"\\sync";
  error_code ec;
  filesystem::create_directories(dstDir, ec);
  ec.clear();
  // 两段枚举：sync\<installation-uuid>\*.userdb.txt，同名字典取 mtime 最新的
  // 一份（每次导出都会覆写自己 uuid 下的文件）。
  std::map<std::wstring, std::pair<FILETIME, std::wstring>> newest;
  WIN32_FIND_DATAW sub = {};
  HANDLE hSub = ::FindFirstFileW((srcSync + L"\\*").c_str(), &sub);
  if (hSub != INVALID_HANDLE_VALUE) {
    do {
      if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
      if (sub.cFileName[0] == L'.') continue;
      // 跳过从主词典导入的副本（master-<来源>）：那是"别人学会的词"，
      // 再导出回主词典会形成回声（自己的快照被别人反复合并）。
      if (wcsncmp(sub.cFileName, L"master-", 7) == 0) continue;
      const wstring subDir = srcSync + L"\\" + sub.cFileName;
      WIN32_FIND_DATAW f2 = {};
      HANDLE hF2 = ::FindFirstFileW((subDir + L"\\*.userdb.txt").c_str(), &f2);
      if (hF2 == INVALID_HANDLE_VALUE) continue;
      do {
        const wstring srcPath = subDir + L"\\" + f2.cFileName;
        auto& best = newest[f2.cFileName];
        if (best.second.empty() ||
            CompareFileTime(&f2.ftLastWriteTime, &best.first) > 0) {
          best = {f2.ftLastWriteTime, srcPath};
        }
      } while (::FindNextFileW(hF2, &f2));
      ::FindClose(hF2);
    } while (::FindNextFileW(hSub, &sub));
    ::FindClose(hSub);
  }
  int copied = 0;
  for (const auto& kv : newest) {
    error_code copyEc;
    filesystem::copy(kv.second.second, dstDir + L"\\" + kv.first,
                     filesystem::copy_options::overwrite_existing, copyEc);
    if (!copyEc) {
      ++copied;
    }
  }
  if (copied > 0) {
    appendLocalEngineLog(L"[userdb] exported " + to_wstring(copied) +
                         L" snapshot(s) to master sync dir");
  }
  return copied;
}

// 反向导入：主词典 sync/<p-*>/ 下是各进程导出的快照。把它们拷进本进程
// 用户目录的 sync\master-import\ 再 RimeSyncUserData，即可把"别的应用刚
// 学会的词"合并进本进程 userdb（2026-11-03 用户实报跨进程不共享）。
bool importMasterUserDbSnapshots() {
  RimeRuntime& runtime = RimeRuntime::instance();
  if (!runtime.ready() || runtime.rimeSyncUserData == nullptr ||
      runtime.rimeJoinMaintenanceThread == nullptr) {
    return false;
  }
  const std::wstring userDir = runtime.inprocUserDir();
  const std::wstring root = moqiAppDataDir();
  if (userDir.empty() || root.empty()) return false;
  const std::wstring masterSync = root + L"\\UserDictMaster\\sync";
  const std::wstring syncDir = userDir + L"\\sync";
  error_code ec;
  filesystem::create_directories(syncDir, ec);
  ec.clear();

  // rime 的 Synchronize 只扫描 sync/<一级子目录>/<dict>.userdb.txt —— 每个
  // 来源必须是 sync 的直接子目录、文件名保持原样（平铺或多一层嵌套都会被
  // 忽略，2026-11-03 实测）。子目录名加 master- 前缀与自有 uuid 目录区分。
  int copied = 0;
  WIN32_FIND_DATAW sub = {};
  HANDLE hSub = ::FindFirstFileW((masterSync + L"\\*").c_str(), &sub);
  if (hSub != INVALID_HANDLE_VALUE) {
    do {
      if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
      if (sub.cFileName[0] == L'.') continue;
      const wstring subDir = masterSync + L"\\" + sub.cFileName;
      WIN32_FIND_DATAW f2 = {};
      HANDLE hF2 = ::FindFirstFileW((subDir + L"\\*.userdb.txt").c_str(), &f2);
      if (hF2 == INVALID_HANDLE_VALUE) continue;
      const wstring dstDir = syncDir + L"\\master-" + sub.cFileName;
      filesystem::create_directories(dstDir, ec);
      do {
        error_code copyEc;
        filesystem::copy(subDir + L"\\" + f2.cFileName,
                         dstDir + L"\\" + f2.cFileName,
                         filesystem::copy_options::overwrite_existing, copyEc);
        if (!copyEc) ++copied;
      } while (::FindNextFileW(hF2, &f2));
      ::FindClose(hF2);
    } while (::FindNextFileW(hSub, &sub));
    ::FindClose(hSub);
  }
  if (copied == 0) return false;

  bool ok = false;
  {
    std::lock_guard<std::recursive_mutex> lock(runtime.mutex());
    // 首次接管（全新进程目录）时本进程还没有 rime_frost.userdb：RimeSyncUserData
    // 内部按目录枚举 *.userdb，找不到就"合并 0 个词典"直接返回成功（2026-11-03
    // 实测日志 synchronizing 0 user dicts）——快照全被静默跳过，用户词永远进不来。
    // 先起一个临时会话把 userdb 建出来（rimeCreateSession 会加载方案并创建
    // userdb 元数据），销毁后再同步。
    if (runtime.rimeCreateSession != nullptr &&
        runtime.rimeDestroySession != nullptr) {
      rimeapi::SessionId tmp = runtime.rimeCreateSession();
      if (tmp != 0) {
        runtime.rimeDestroySession(tmp);
      }
    }
    ok = runtime.rimeSyncUserData() != 0;
    runtime.rimeJoinMaintenanceThread();
  }
  appendLocalEngineLog(ok ? L"[userdb] imported " + to_wstring(copied) +
                                L" snapshot(s) from master"
                          : L"[userdb] import RimeSyncUserData failed");
  return ok;
}

} // namespace Moqi
