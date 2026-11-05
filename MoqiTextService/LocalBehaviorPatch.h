// LocalBehaviorPatch.h —— 产品行为补丁体的纯生成逻辑（原 RimeCore.cpp 内联）。
// 独立成头是为了可单测：local_behavior_patch_test.cpp 锁定每一行输出。
// 背景（10-26 事故）：产品配置与白盒考卷 yaml 脱节曾让 d3 假摔 8 点——
// 本函数的输出就是产品配置的唯一权威，任何改动必须先过这里的锁定用例。
#ifndef MOQI_LOCAL_BEHAVIOR_PATCH_H_
#define MOQI_LOCAL_BEHAVIOR_PATCH_H_

#include <json/json.h>

#include <cstdio>
#include <string>

namespace Moqi {

// 参数：
//   cfg        = local_behavior.json（缺字段 = 关闭/默认）
//   contextual = contextual_config.json（只读 bigram_rerank.mixed_pair）
//   lang       = 最终语法模型语言（空 = 无 gram：不输出任何 grammar/typo 行）
// 返回：完整 patch YAML 文本；无任何输出行时返回空串（调用方据此跳过落盘）。
inline std::string buildLocalBehaviorPatchBody(const Json::Value& cfg,
                                               const Json::Value& contextual,
                                               const std::string& lang) {
  std::string body = "# managed by moqi local engine\npatch:\n";
  int lines = 0;
  auto emit = [&](const std::string& line) {
    body += "  " + line + "\n";
    ++lines;
  };
  auto jsonBool = [&](const char* key) {
    // cfg 非对象（手改坏成数组/标量）时 operator[] 会抛——按全关处理。
    if (!cfg.isObject()) return false;
    const Json::Value& x = cfg[key];
    return x.isBool() && x.asBool();
  };

  if (jsonBool("disable_slash_punct")) {
    emit("recognizer/patterns/punct: null");
  } else if (jsonBool("slash_punct_v_only")) {
    emit("recognizer/patterns/punct: \"^v([0-9]|10|[A-Za-z]+)$\"");
  }
  if (jsonBool("disable_unicode_trigger")) {
    emit("recognizer/patterns/unicode: null");
  }
  if (jsonBool("disable_number_trigger")) {
    emit("recognizer/patterns/number: null");
  }
  if (jsonBool("disable_lunar_trigger")) {
    emit("recognizer/patterns/gregorian_to_lunar: null");
  }
  if (jsonBool("disable_calculator")) {
    emit("recognizer/patterns/calculator: null");
  }
  if (jsonBool("disable_aux_lookup")) {
    emit("engine/translators/-: table_translator@frost_aux");
    emit("engine/filters/-: lua_filter@*aux_lookup_filter");
  }
  // MixedPair 默认开（Go defaultBigramRerankConfig）；关闭时不再禁用学习。
  // contextual 非对象（手改坏）按默认走，绝不抛。
  bool rerankMixed = true;
  if (contextual.isObject()) {
    const Json::Value& br = contextual["bigram_rerank"];
    if (br.isObject() && br["mixed_pair"].isBool()) {
      rerankMixed = br["mixed_pair"].asBool();
    }
  }
  if (rerankMixed) {
    emit("translator/disable_user_dict_for_patterns: "
         "[\".*[bcdfhjklmpqstvwxyz]$\"]");
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
    } else if (lang.find("wanxiang") != std::string::npos) {
      penalty = 0;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "grammar/non_collocation_penalty: %g",
                  penalty);
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

}  // namespace Moqi

#endif  // MOQI_LOCAL_BEHAVIOR_PATCH_H_