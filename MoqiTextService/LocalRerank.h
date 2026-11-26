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
  // 整句搭配证据通道 v3（2026-11-20）：判别量=PMI 强搭配对数——
  // ln(count·N/(mass[a]·mass[b])) ≥ collocPmi 计一对"凝固搭配"。v1 有据
  // 对数/v2 等长闸两轮实炸（病句靠泛用字链，如 不知道→我 PMI 0.90）；
  // 数据实测：正解搭配 PMI 3.66~5.75，病句 ≤1.59，阈值 2.0 一刀切开。
  // 默认 2.0 开启（2026-11-20 用户拍板"真歧义可以接受，比无比解决好太多"）：
  // v3 回放 710/750（-1 词），修好 4 病案（时间推移/我想说的是/魔幻现实
  // 主义/为什么这么样）+ 案件一务必；新错 5 个全部是双重凝固搭配真歧义
  //（最重要的角色 vs 就是），泛用字链（不知道我是谁）已被 PMI 全部分离。
  double collocPmi = 2.0;
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
  // zh-bigram 全表 token 总量（PMI 归一化用）。
  long long totalBigramTokens() const { return totalTokens_; }

  // 测试接缝（local_rerank_test）：直接注入配置与表，绕过文件加载；
  // tsvLoaded_=true 防止后续 ensureLoaded 重读 tsv 覆盖注入内容。
  // totalTokens 缺省 0（旧用例只测 mixedPair 不看 PMI 归一化）；
  // colloc 用例必须显式给正数，否则 collocScore 因 total<=0 整体返回 0。
  void injectForTest(
      const RerankConfig& cfg,
      std::unordered_map<std::string, std::unordered_map<std::string, int>> pairs,
      std::unordered_map<std::string, int> wordMass,
      long long totalTokens = 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
    pairCounts_ = std::move(pairs);
    wordMass_ = std::move(wordMass);
    totalTokens_ = totalTokens;
    tsvLoaded_ = true;
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
  long long totalTokens_ = 0;
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
// 整句搭配证据换位（只在组字串完全可拼分时生效）：mixedPair 未动时，用
// bigram 后继表的"有据对数优势"翻罚分杀不动的固定搭配病句。
bool applyCollocationRerank(const std::string& composition,
                            std::vector<Cand>& candidates, int& cursor);

// Display-position -> engine-position map (nil-equivalent: empty when order
// is unchanged or texts do not biject).
std::vector<int> buildDisplayPerm(const std::vector<Cand>& engineOrder,
                                  const std::vector<Cand>& display);

// 错字记忆（typo_memory.tsv 与 Go 后端共用同一文件；mtime+TTL 增量重载）。
class TypoMemoryStore {
public:
  static TypoMemoryStore& instance();
  ~TypoMemoryStore();
  void ensureLoaded(const std::string& appDataRoot);
  void record(const std::string& raw, const std::string& text);
  // Persist pending rows at session boundaries, outside the per-key hot path.
  void flush();
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
