//
//	LocalRerank.cpp: C++ port of the Go hot-path rerank features
//	(contextual_lift.go / bigram_rerank.go / typo_memory.go) for the
//	in-process engine. Algorithms and thresholds mirror the Go code
//	one-to-one; data files are shared with the backend.
//
#include "LocalRerank.h"
#include "libIME2/src/Utils.h"

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <json/json.h>
#include <memory>
#include <sstream>
#include <unordered_set>

using namespace std;

namespace Moqi {

namespace {

// --------------------------------------------------------- utf-8 helpers --

size_t runeLen(unsigned char c) {
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 拼音音节表（生成自 Go 源，勿手改）。
const unordered_set<string>& pinyinSyllables() {
  static const unordered_set<string> set = {
#include "PinyinSyllables.inc"
  };
  return set;
}

bool segmentable(const string& tok) {
  const size_t n = tok.size();
  if (n == 0) return false;
  vector<bool> dp(n + 1, false);
  dp[0] = true;
  for (size_t j = 1; j <= n; ++j) {
    for (size_t i = j; i-- > 0;) {
      if (j - i > 6) break;
      if (dp[i] && pinyinSyllables().count(tok.substr(i, j - i)) != 0) {
        dp[j] = true;
        break;
      }
    }
  }
  return dp[n];
}

bool fullyPinyinSegmentable(const string& s) {
  istringstream stream(s);
  string tok;
  bool any = false;
  while (stream >> tok) {
    any = true;
    if (!segmentable(tok)) return false;
  }
  return any;
}

// 组字串候选打分：所有切分点取 max pairCounts[前段][后段]。
int pairScore(const unordered_map<string, unordered_map<string, int>>& pairs,
              const string& text, int maxLen) {
  const size_t runes = utf8RuneCount(text);
  if (runes < 2 || runes > static_cast<size_t>(maxLen)) return 0;
  int best = 0;
  for (size_t l = 1; l < runes; ++l) {
    string head = utf8RunePrefix(text, l);
    auto it = pairs.find(head);
    if (it == pairs.end()) continue;
    auto cit = it->second.find(utf8RuneSuffix(text, static_cast<int>(runes - l)));
    if (cit != it->second.end() && cit->second > best) best = cit->second;
  }
  return best;
}

// stripMarks：候选注释/标记前的正文（Go 版按首个空格截断再 trim）。
string stripMarks(const string& s) {
  const size_t sp = s.find(' ');
  string head = sp == string::npos ? s : s.substr(0, sp);
  const size_t b = head.find_first_not_of(" \t");
  if (b == string::npos) return "";
  const size_t e = head.find_last_not_of(" \t");
  return head.substr(b, e - b + 1);
}

// 首名提升式换位（Go: copy(reordered[1:bestIdx+1], reordered[0:bestIdx])）。
void rotateToFirst(vector<Cand>& candidates, size_t bestIdx) {
  const Cand winner = candidates[bestIdx];
  for (size_t i = bestIdx; i > 0; --i) {
    candidates[i] = candidates[i - 1];
  }
  candidates[0] = winner;
}

uint64_t fileMtime(const string& pathUtf8) {
  WIN32_FILE_ATTRIBUTE_DATA info = {};
  const wstring wide = utf8ToUtf16(pathUtf8.c_str());
  if (!::GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &info)) {
    return 0;
  }
  return (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
         info.ftLastWriteTime.dwLowDateTime;
}

string readWholeFile(const string& pathUtf8) {
  ifstream file(utf8ToUtf16(pathUtf8.c_str()), ios::binary);
  if (!file) return "";
  ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

} // namespace

size_t utf8RuneCount(const string& s) {
  size_t runes = 0;
  for (size_t i = 0; i < s.size();) {
    i += runeLen(static_cast<unsigned char>(s[i]));
    ++runes;
  }
  return runes;
}

string utf8FirstRune(const string& s) {
  if (s.empty()) return "";
  return s.substr(0, runeLen(static_cast<unsigned char>(s[0])));
}

string utf8RuneSuffix(const string& s, int runes) {
  if (runes <= 0) return "";
  const size_t total = utf8RuneCount(s);
  if (static_cast<size_t>(runes) >= total) return s;
  size_t skip = total - static_cast<size_t>(runes);
  size_t i = 0;
  while (skip > 0 && i < s.size()) {
    i += runeLen(static_cast<unsigned char>(s[i]));
    --skip;
  }
  return s.substr(i);
}

string utf8RunePrefix(const string& s, size_t runes) {
  size_t i = 0, seen = 0;
  while (i < s.size() && seen < runes) {
    i += runeLen(static_cast<unsigned char>(s[i]));
    ++seen;
  }
  return s.substr(0, i);
}

// ============================================================ RerankData ==

RerankData& RerankData::instance() {
  static RerankData data;
  return data;
}

namespace {

// 内置接续表（与 contextual_lift.go 完全一致，宁缺毋滥）。
void installDefaultContinuations(RerankConfig& cfg) {
  cfg.continuations.clear();
  auto add = [&cfg](const char* tail, initializer_list<const char*> conts) {
    cfg.continuations[tail] = vector<string>(conts.begin(), conts.end());
  };
  add("你好", {"吗"});
  add("谢谢", {"你"});
  add("辛苦", {"了"});
  add("请问", {});
  add("多", {"谢"});
  add("很", {"好"});
  add("不", {"错"});
  add("没", {"事", "有"});
  add("我", {"们"});
  add("你", {"好"});
  add("他", {"们"});
  add("她", {"们"});
  add("在", {"哪", "这"});
  add("哪", {"里"});
  add("是", {"的", "吗"});
  add("不是", {"吗"});
  add("对", {"的", "吗"});
  add("可以", {"吗"});
  add("好", {"的", "吗"});
  add("真", {"的", "好"});
  add("太", {"好", "多"});
  add("就", {"是"});
  add("都", {"是"});
  add("也", {"是", "好"});
  add("还", {"好", "是"});
  add("什么", {"呢"});
  add("为什么", {"呢"});
  add("怎么", {"样", "办"});
  add("吗", {"呢"});
  add("了", {"吗", "吧"});
}

} // namespace

void RerankData::ensureLoaded(const string& sharedDataDir,
                              const string& appDataRoot) {
  lock_guard<mutex> lock(mutex_);
  const string configPath = appDataRoot + "/contextual_lift.json";
  const uint64_t mtime = fileMtime(configPath);
  if (!tsvLoaded_ || mtime != configMtime_) {
    loadLocked(sharedDataDir, appDataRoot);
    configMtime_ = mtime;
  }
}

void RerankData::loadLocked(const string& sharedDataDir,
                            const string& appDataRoot) {
  RerankConfig cfg;
  installDefaultContinuations(cfg);

  // contextual_lift.json：覆盖开关 + 追加接续表 + bigram 子配置。
  const string body = readWholeFile(appDataRoot + "/contextual_lift.json");
  if (!body.empty()) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    unique_ptr<Json::CharReader> reader(builder.newCharReader());
    string errors;
    if (reader->parse(body.data(), body.data() + body.size(), &root,
                      &errors)) {
      if (root["enabled"].isBool()) {
        cfg.contextualEnabled = root["enabled"].asBool();
      }
      if (root.isMember("window_seconds") && root["window_seconds"].isInt() &&
          root["window_seconds"].asInt() >= 0) {
        cfg.windowSeconds = root["window_seconds"].asInt();
      }
      if (root.isMember("max_tail_runes") &&
          root["max_tail_runes"].isInt() && root["max_tail_runes"].asInt() > 0) {
        cfg.maxTailRunes = root["max_tail_runes"].asInt();
      }
      const Json::Value& extra = root["extra_continuations"];
      if (extra.isObject()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) {
          string tail = it.name();
          // trim
          const size_t b = tail.find_first_not_of(" \t");
          const size_t e = tail.find_last_not_of(" \t");
          if (b == string::npos) continue;
          tail = tail.substr(b, e - b + 1);
          if (tail.empty() || !it->isArray() || it->empty()) continue;
          vector<string> conts;
          for (const auto& c : *it) {
            if (c.isString()) conts.push_back(c.asString());
          }
          if (!conts.empty()) cfg.continuations[tail] = conts;
        }
      }
      const Json::Value& br = root["bigram_rerank"];
      if (br.isObject()) {
        if (br["enabled"].isBool()) cfg.bigramEnabled = br["enabled"].asBool();
        if (br.isMember("ratio") && br["ratio"].isDouble() &&
            br["ratio"].asDouble() > 1) {
          cfg.ratio = br["ratio"].asDouble();
        }
        if (br.isMember("floor_count") && br["floor_count"].isInt() &&
            br["floor_count"].asInt() > 0) {
          cfg.floorCount = br["floor_count"].asInt();
        }
        if (br["mixed_pair"].isBool()) {
          cfg.mixedPair = br["mixed_pair"].asBool();
        }
        if (br.isMember("pair_ratio") && br["pair_ratio"].isDouble() &&
            br["pair_ratio"].asDouble() > 1) {
          cfg.pairRatio = br["pair_ratio"].asDouble();
        }
        if (br.isMember("pair_floor") && br["pair_floor"].isInt() &&
            br["pair_floor"].asInt() > 0) {
          cfg.pairFloor = br["pair_floor"].asInt();
        }
        if (br.isMember("pair_max_len") && br["pair_max_len"].isInt() &&
            br["pair_max_len"].asInt() >= 2) {
          cfg.pairMaxLen = br["pair_max_len"].asInt();
        }
        if (br.isMember("pair_word_ratio") &&
            br["pair_word_ratio"].isDouble() &&
            br["pair_word_ratio"].asDouble() >= 0) {
          cfg.pairWordRatio = br["pair_word_ratio"].asDouble();
        }
      }
    }
  }
  cfg_ = move(cfg);

  if (tsvLoaded_) return; // TSV 一次加载即可（与 Go sharedBigramTable 一致）
  tsvLoaded_ = true;

  // zh-bigram.tsv: n\t词\t后词:计数 …
  successorMass_.clear();
  pairCounts_.clear();
  wordMass_.clear();
  const string tsv = readWholeFile(sharedDataDir + "/zh-bigram.tsv");
  size_t pos = 0;
  while (pos < tsv.size()) {
    size_t eol = tsv.find('\n', pos);
    if (eol == string::npos) eol = tsv.size();
    string line = tsv.substr(pos, eol - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pos = eol + 1;
    if (line.compare(0, 2, "n\t") != 0) continue;
    const size_t t1 = 1;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == string::npos) continue;
    const string w1 = line.substr(t1 + 1, t2 - t1 - 1);
    auto& row = successorMass_[w1];
    auto& prow = pairCounts_[w1];
    int rowTotal = 0;
    size_t f = t2 + 1;
    while (f < line.size()) {
      size_t sp = line.find(' ', f);
      if (sp == string::npos) sp = line.size();
      if (sp > f) {
        const string tok = line.substr(f, sp - f);
        const size_t colon = tok.rfind(':');
        if (colon != string::npos && colon > 0) {
          const int c = atoi(tok.c_str() + colon + 1);
          if (c > 0) {
            const string w2 = tok.substr(0, colon);
            if (!w2.empty()) {
              const string first = utf8FirstRune(w2);
              row[first] += c;
              prow[w2] += c;
              wordMass_[w2] += c;
              rowTotal += c;
            }
          }
        }
      }
      f = sp + 1;
    }
    wordMass_[w1] += rowTotal;
  }
}

// ===================================================== ContextualCommits ==

void ContextualCommits::push(const string& commit, int windowSeconds) {
  string text = commit;
  // trim（Go: strings.TrimSpace）
  const size_t b = text.find_first_not_of(" \t\r\n");
  if (b == string::npos) return;
  const size_t e = text.find_last_not_of(" \t\r\n");
  text = text.substr(b, e - b + 1);
  if (text.empty()) return;

  const ULONGLONG now = ::GetTickCount64();
  const ULONGLONG windowMs =
      static_cast<ULONGLONG>(windowSeconds) * 1000ULL;
  deque<Record> keep;
  for (const auto& rec : records_) {
    if (now - rec.tick < windowMs) keep.push_back(rec);
  }
  keep.push_back({text, now});
  while (keep.size() > 2) keep.pop_front();
  records_ = move(keep);
}

bool ContextualCommits::activeTail(int windowSeconds, string* text) const {
  if (records_.empty()) return false;
  const Record& rec = records_.back();
  if (::GetTickCount64() - rec.tick >=
      static_cast<ULONGLONG>(windowSeconds) * 1000ULL) {
    return false;
  }
  *text = rec.text;
  return true;
}

// ==================================================== rerank 算法（Go 同款）==

bool applyContextualLift(const string& commitText, vector<Cand>& candidates,
                         int& cursor) {
  const RerankConfig& cfg = RerankData::instance().config();
  if (!cfg.contextualEnabled || candidates.empty() || commitText.empty()) {
    return false;
  }
  const string commit = commitText; // caller passes trimmed active tail
  size_t maxTail = static_cast<size_t>(cfg.maxTailRunes);
  const size_t runeCount = utf8RuneCount(commit);
  if (maxTail > runeCount) maxTail = runeCount;
  for (size_t l = maxTail; l >= 1; --l) {
    const string tail = utf8RuneSuffix(commit, static_cast<int>(l));
    auto it = cfg.continuations.find(tail);
    if (it == cfg.continuations.end()) continue;
    for (const string& cont : it->second) {
      const string want = utf8FirstRune(cont);
      if (want.empty()) continue;
      size_t matched = 0;
      for (const auto& item : candidates) {
        if (utf8FirstRune(item.first) == want) ++matched;
      }
      if (matched == 0 || matched == candidates.size()) continue;
      vector<Cand> matchedList, rest;
      matchedList.reserve(matched);
      for (auto& item : candidates) {
        if (utf8FirstRune(item.first) == want) {
          matchedList.push_back(item);
        } else {
          rest.push_back(item);
        }
      }
      vector<Cand> reordered = move(matchedList);
      reordered.insert(reordered.end(), make_move_iterator(rest.begin()),
                       make_move_iterator(rest.end()));
      // 光标跟随原指向的候选文本。
      if (cursor >= 0 && cursor < static_cast<int>(candidates.size())) {
        const string oldText = candidates[cursor].first;
        for (size_t i = 0; i < reordered.size(); ++i) {
          if (reordered[i].first == oldText) {
            cursor = static_cast<int>(i);
            break;
          }
        }
      }
      candidates = move(reordered);
      return true;
    }
  }
  return false;
}

bool applyBigramRerank(const string& commitText, vector<Cand>& candidates,
                       int& cursor) {
  const RerankData& data = RerankData::instance();
  const RerankConfig& cfg = data.config();
  if (!cfg.bigramEnabled || candidates.size() < 2 || commitText.empty()) {
    return false;
  }
  // successorMass：最近尾词 1..4 字后缀最长命中。
  const auto& table = data.successorMass();
  if (table.empty()) return false;
  const unordered_map<string, int>* mass = nullptr;
  const size_t runeCount = utf8RuneCount(commitText);
  for (size_t l = 4; l >= 1; --l) {
    if (l > runeCount) continue;
    auto it = table.find(utf8RuneSuffix(commitText, static_cast<int>(l)));
    if (it != table.end()) {
      mass = &it->second;
      break;
    }
  }
  if (mass == nullptr) return false;

  auto score = [mass](const Cand& item) -> int {
    const string first = utf8FirstRune(item.first);
    if (first.empty()) return 0;
    auto it = mass->find(first);
    return it == mass->end() ? 0 : it->second;
  };
  const int firstScore = score(candidates[0]);
  int bestIdx = -1;
  for (size_t i = 1; i < candidates.size() && i < 3; ++i) {
    const int s = score(candidates[i]);
    if (s >= cfg.floorCount &&
        (firstScore == 0 ||
         static_cast<double>(s) > cfg.ratio * static_cast<double>(firstScore))) {
      if (bestIdx < 0 || s > score(candidates[bestIdx])) {
        bestIdx = static_cast<int>(i);
      }
    }
  }
  if (bestIdx < 0) return false;
  rotateToFirst(candidates, static_cast<size_t>(bestIdx));
  if (cursor == bestIdx) {
    cursor = 0;
  } else if (cursor == 0) {
    cursor = bestIdx;
  }
  return true;
}

bool applyMixedPairRerank(const string& composition, vector<Cand>& candidates,
                          int& cursor) {
  const RerankData& data = RerankData::instance();
  const RerankConfig& cfg = data.config();
  if (!cfg.mixedPair || candidates.size() < 2 ||
      fullyPinyinSegmentable(composition)) {
    return false;
  }
  const auto& pairs = data.pairCounts();
  const auto& wordMass = data.wordMass();
  if (pairs.empty()) return false;
  const int maxLen = cfg.pairMaxLen < 2 ? 2 : cfg.pairMaxLen;

  const int first = pairScore(pairs, candidates[0].first, maxLen);
  int wordMassAbove = 0;
  int bestIdx = -1;
  int bestScore = 0;
  for (size_t i = 1; i < candidates.size(); ++i) {
    auto wm = wordMass.find(stripMarks(candidates[i - 1].first));
    if (wm != wordMass.end() && wm->second > wordMassAbove) {
      wordMassAbove = wm->second;
    }
    const int s = pairScore(pairs, candidates[i].first, maxLen);
    if (s < cfg.pairFloor) continue;
    if (first != 0 &&
        static_cast<double>(s) < cfg.pairRatio * static_cast<double>(first)) {
      continue;
    }
    if (static_cast<double>(s) <
        cfg.pairWordRatio * static_cast<double>(wordMassAbove)) {
      continue;
    }
    if (s > bestScore) {
      bestScore = s;
      bestIdx = static_cast<int>(i);
    }
  }
  if (bestIdx < 0) return false;
  rotateToFirst(candidates, static_cast<size_t>(bestIdx));
  if (cursor == bestIdx) cursor = 0;
  return true;
}

vector<int> buildDisplayPerm(const vector<Cand>& engineOrder,
                             const vector<Cand>& display) {
  vector<int> perm;
  if (engineOrder.empty() || display.empty() ||
      engineOrder.size() != display.size()) {
    return perm;
  }
  perm.resize(display.size());
  vector<bool> used(engineOrder.size(), false);
  bool sameOrder = true;
  for (size_t di = 0; di < display.size(); ++di) {
    int found = -1;
    for (size_t ei = 0; ei < engineOrder.size(); ++ei) {
      if (!used[ei] && engineOrder[ei].first == display[di].first) {
        found = static_cast<int>(ei);
        break;
      }
    }
    if (found < 0) return vector<int>(); // 文本不双射：退化为同序
    used[found] = true;
    perm[di] = found;
    if (found != static_cast<int>(di)) sameOrder = false;
  }
  if (sameOrder) return vector<int>();
  return perm;
}

// ======================================================== TypoMemoryStore ==

namespace {
constexpr const char* kTypoMemoryFileName = "typo_memory.tsv";
constexpr size_t kTypoMaxEntries = 5000;
constexpr size_t kTypoMinRawLen = 3;
constexpr int kTypoSaveEvery = 8;
constexpr ULONGLONG kTypoReloadMs = 5ULL * 60ULL * 1000ULL; // 5 分钟
} // namespace

TypoMemoryStore& TypoMemoryStore::instance() {
  static TypoMemoryStore store;
  return store;
}

void TypoMemoryStore::ensureLoaded(const string& appDataRoot) {
  lock_guard<mutex> lock(mutex_);
  path_ = appDataRoot + "/" + kTypoMemoryFileName;
  if (loadedOnce_) return;
  loadedOnce_ = true;
  loadLocked(true);
}

void TypoMemoryStore::loadLocked(bool force) {
  const ULONGLONG now = ::GetTickCount64();
  if (!force && now - lastLoadedTick_ < kTypoReloadMs) return;
  lastLoadedTick_ = now;
  if (path_.empty()) return;
  const uint64_t mtime = fileMtime(path_);
  if (mtime == 0) return;
  if (loadedMtime_ != 0 && mtime <= loadedMtime_) return;
  ifstream file(utf8ToUtf16(path_.c_str()), ios::binary);
  if (!file) return;
  unordered_map<string, pair<int, string>> entries;
  string line;
  while (getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t t1 = line.find('\t');
    if (t1 == string::npos) continue;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == string::npos) continue;
    const int n = atoi(line.c_str());
    const string raw = line.substr(t1 + 1, t2 - t1 - 1);
    const string text = line.substr(t2 + 1);
    if (n <= 0 || raw.empty() || text.empty()) continue;
    auto it = entries.find(raw);
    if (it == entries.end() || n > it->second.first) {
      entries[raw] = {n, text};
    }
  }
  entries_ = move(entries);
  loadedMtime_ = mtime;
  dirty_ = 0;
}

void TypoMemoryStore::saveLocked() {
  if (path_.empty() || entries_.empty()) return;
  vector<pair<int, pair<string, string>>> rows; // {count, {raw, text}}
  rows.reserve(entries_.size());
  for (const auto& e : entries_) {
    rows.push_back({e.second.first, {e.first, e.second.second}});
  }
  sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second.first < b.second.first;
  });
  if (rows.size() > kTypoMaxEntries) rows.resize(kTypoMaxEntries);
  ostringstream out;
  for (const auto& r : rows) {
    out << r.first << '\t' << r.second.first << '\t' << r.second.second
        << '\n';
  }
  const string body = out.str();
  const wstring dir = utf8ToUtf16(path_.substr(0, path_.find_last_of('/'))
                                      .c_str());
  ::CreateDirectoryW(dir.c_str(), nullptr);
  ofstream file(utf8ToUtf16(path_.c_str()),
                ios::binary | ios::trunc);
  if (!file) return;
  file.write(body.data(), static_cast<streamsize>(body.size()));
  const uint64_t mtime = fileMtime(path_);
  if (mtime != 0) loadedMtime_ = mtime;
  dirty_ = 0;
}

void TypoMemoryStore::record(const string& rawIn, const string& textIn) {
  auto trim = [](const string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  };
  const string raw = trim(rawIn);
  const string text = trim(textIn);
  if (raw.empty() || text.empty()) return;
  for (char c : raw) {
    if (c < 'a' || c > 'z') return;
  }
  if (raw.size() < kTypoMinRawLen) return;
  if (utf8RuneCount(text) < 1) return;

  lock_guard<mutex> lock(mutex_);
  auto it = entries_.find(raw);
  if (it != entries_.end()) {
    if (it->second.second == text) {
      it->second.first += 1;
    } else {
      it->second.second = text;
      it->second.first = it->second.first / 2 + 1;
    }
  } else {
    entries_[raw] = {1, text};
  }
  if (++dirty_ >= kTypoSaveEvery) saveLocked();
}

string TypoMemoryStore::lookup(const string& raw) {
  lock_guard<mutex> lock(mutex_);
  // TTL 内直接内存命中；过期时先重载（loadLocked 自带 mtime 去重）。
  loadLocked(false);
  auto it = entries_.find(raw);
  return it != entries_.end() ? it->second.second : string();
}

} // namespace Moqi
