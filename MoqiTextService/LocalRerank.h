//
//	LocalRerank: phase-2 hot-path quality features ported from the Go
//	backend (contextual_lift.go / bigram_rerank.go / typo_memory.go), so
//	the in-process engine's candidate ordering matches the RPC path.
//
#ifndef MOQI_LOCAL_RERANK_H_
#define MOQI_LOCAL_RERANK_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Moqi {

// UTF-8 helpers (byte-space matching mirrors Go, which works on UTF-8
// strings natively).
size_t utf8RuneCount(const std::string& s);
std::string utf8FirstRune(const std::string& s); // "" when empty
std::string utf8RuneSuffix(const std::string& s, int runes); // last N runes
std::string utf8RunePrefix(const std::string& s, size_t runes); // first N runes

// Candidate text + comment, the LocalEngine representation.
typedef std::pair<std::string, std::string> Cand;

struct RerankConfig {
  bool contextualEnabled = true;
  int windowSeconds = 8;
  int maxTailRunes = 4;
  bool bigramEnabled = false;
  double ratio = 1.6;
  int floorCount = 30;
  bool mixedPair = true;
  double pairRatio = 999.0;
  int pairFloor = 300;
  int pairMaxLen = 4;
  double pairWordRatio = 1.0;
  std::unordered_map<std::string, std::vector<std::string>> continuations;
};

// Process-wide config + bigram tables (mirrors sharedBigramTable and
// loadContextualConfig). Loads defaults, then contextual_lift.json (user
// config, reloaded when its mtime changes) and zh-bigram.tsv (once).
class RerankData {
public:
  static RerankData& instance();

  // sharedDataDir: install ...\rime\data (zh-bigram.tsv);
  // appDataRoot: %APPDATA%\Moqi (contextual_lift.json).
  void ensureLoaded(const std::string& sharedDataDir,
                    const std::string& appDataRoot);

  const RerankConfig& config() const { return cfg_; }
  const std::unordered_map<std::string, std::unordered_map<std::string, int>>&
  successorMass() const {
    return successorMass_;
  }
  const std::unordered_map<std::string, std::unordered_map<std::string, int>>&
  pairCounts() const {
    return pairCounts_;
  }
  const std::unordered_map<std::string, int>& wordMass() const {
    return wordMass_;
  }

private:
  RerankData() = default;
  void loadLocked(const std::string& sharedDataDir,
                  const std::string& appDataRoot);

  std::mutex mutex_;
  bool tsvLoaded_ = false;
  uint64_t configMtime_ = 0;
  RerankConfig cfg_;
  std::unordered_map<std::string, std::unordered_map<std::string, int>>
      successorMass_;
  std::unordered_map<std::string, std::unordered_map<std::string, int>>
      pairCounts_;
  std::unordered_map<std::string, int> wordMass_;
};

// 短窗连贯：最近 8 秒上屏句子的尾词接续提升（applyContextualLift 的状态侧）。
class ContextualCommits {
public:
  void push(const std::string& commit, int windowSeconds);
  // Latest unexpired commit text; false when none.
  bool activeTail(int windowSeconds, std::string* text) const;

private:
  struct Record {
    std::string text;
    uint64_t tick;
  };
  std::deque<Record> records_; // most recent last, max 2
};

// The three rerank stages; each mirrors its Go counterpart and reports
// whether it reordered (cursor follows the previously highlighted text).
bool applyContextualLift(const std::string& commitText,
                         std::vector<Cand>& candidates, int& cursor);
bool applyBigramRerank(const std::string& commitText,
                       std::vector<Cand>& candidates, int& cursor);
bool applyMixedPairRerank(const std::string& composition,
                          std::vector<Cand>& candidates, int& cursor);

// Display-position -> engine-position map (nil-equivalent: empty when order
// is unchanged or texts do not biject).
std::vector<int> buildDisplayPerm(const std::vector<Cand>& engineOrder,
                                  const std::vector<Cand>& display);

// 错字记忆（typo_memory.tsv 与 Go 后端共用同一文件；mtime+TTL 增量重载）。
class TypoMemoryStore {
public:
  static TypoMemoryStore& instance();
  void ensureLoaded(const std::string& appDataRoot);
  void record(const std::string& raw, const std::string& text);
  std::string lookup(const std::string& raw);

private:
  TypoMemoryStore() = default;
  void loadLocked(bool force);
  void saveLocked();

  std::mutex mutex_;
  std::string path_;
  std::unordered_map<std::string, std::pair<int, std::string>> entries_; // raw -> {count, text}
  int dirty_ = 0;
  uint64_t lastLoadedTick_ = 0;
  uint64_t loadedMtime_ = 0;
  bool loadedOnce_ = false;
};

} // namespace Moqi

#endif // MOQI_LOCAL_RERANK_H_
