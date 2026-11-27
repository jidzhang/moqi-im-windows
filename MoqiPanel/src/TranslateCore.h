#pragma once
// 翻译配置与应答解析（纯逻辑，jsoncpp 之外不依赖任何东西）。
// include 纪律：windows.h 开篇（见 PanelCore.cpp）。
#include <windows.h>

#include <filesystem>
#include <string>

#include <json/json.h>

namespace MoqiPanel {

// translate.json（与 MoqiPanel.exe 同目录）的内存形态。
struct TranslateConfig {
  std::string baseUrl;     // OpenAI 兼容接口地址，三种写法都接受，见 chatCompletionsUrl
  std::string apiKey;      // 可空（本地推理服务通常不需要密钥）
  std::string model;
  std::string proxy;       // 空 = 系统代理；"direct" = 直连；"http://主机:端口" = 指定代理
  std::string systemPrompt;  // 空 = 内置提示词
  int maxTokens = 0;       // 0 = 不下发（推理模型对 max_tokens 挑剔）
  Json::Value extraBody;   // 附加请求参数（如关闭思考），逐键并入请求体

  bool configured() const { return !baseUrl.empty() && !model.empty(); }
};

// 读配置。文件不存在 = 未配置（返回 true、字段全空）；JSON 写坏或类型不对 =
// 返回 false 并给出中文原因。绝不抛异常——配置是用户手改的。
bool loadTranslateConfig(const std::filesystem::path& file, TranslateConfig* out,
                         std::wstring* error);

// 写配置（UTF-8 无 BOM）：先写 .tmp 再原子替换，写一半也不会留下坏文件。
bool saveTranslateConfig(const std::filesystem::path& file,
                         const TranslateConfig& config);

// 首次运行生成一份模板（只有文件不存在时才写）。
void ensureTranslateConfigTemplate(const std::filesystem::path& file);

// 把用户填的接口地址统一成请求用的完整地址：
//   https://api.deepseek.com | https://api.deepseek.com/v1 | …/chat/completions
std::string chatCompletionsUrl(std::string baseUrl);

std::string defaultSystemPrompt();

// chat/completions 请求体：model + messages（+ 可选 max_tokens）+ extraBody。
std::string buildTranslateRequest(const TranslateConfig& config,
                                  const std::string& utf8Text);

// 从应答取译文。content 是字符串或分段数组都接受；失败时 error 给出可读原因。
bool parseChatText(const std::string& responseBody, std::string* text,
                   std::string* error);

// 非 2xx 应答的可读原因；能从应答体里挖出 message 就带上。
std::string describeHttpError(int status, const std::string& responseBody);

} // namespace MoqiPanel
