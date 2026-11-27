#pragma once
// 翻译的网络层：OpenAI 兼容的 /chat/completions（同步，调用方放后台线程）。
// 面板自己直连接口、自己读 translate.json——不借 daemon 的 AI 评审通道：
// daemon 的分发锁是全局串行的，HTTP 放进去会把所有按键卡住。
#include <windows.h>

#include <string>

#include "TranslateCore.h"

namespace MoqiPanel {

struct TranslateOutcome {
  bool ok = false;
  std::string translation;  // UTF-8，已整理
  std::string error;        // UTF-8 的中文原因
};

// 读配置 → 组请求 → POST → 解析。任何一步失败都只填 error，不抛异常。
TranslateOutcome translateText(const TranslateConfig& config,
                               const std::string& utf8Text);

} // namespace MoqiPanel
