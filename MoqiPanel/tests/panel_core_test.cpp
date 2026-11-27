// 面板纯逻辑回归：标点映射逐条锁定、字数护栏、配置解析的类型安全、
// 请求组装、应答解析。无第三方框架；任一断言失败即非零退出。
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "PanelCore.h"
#include "TranslateCore.h"

namespace {

int g_checks = 0;
int g_failures = 0;

std::string narrow(const std::wstring& text) { return MoqiPanel::wideToUtf8(text); }

void check(bool ok, const char* file, int line, const char* expression) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s:%d  %s\n", file, line, expression);
  }
}

void checkWide(const std::wstring& actual, const std::wstring& expected, const char* file,
               int line) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::printf("FAIL %s:%d\n  expected: %s\n  actual:   %s\n", file, line,
                narrow(expected).c_str(), narrow(actual).c_str());
  }
}

void checkNarrow(const std::string& actual, const std::string& expected, const char* file,
                 int line) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::printf("FAIL %s:%d\n  expected: %s\n  actual:   %s\n", file, line,
                expected.c_str(), actual.c_str());
  }
}

#define CHECK(expr) check((expr), __FILE__, __LINE__, #expr)
#define CHECK_WIDE(actual, expected) checkWide((actual), (expected), __FILE__, __LINE__)
#define CHECK_NARROW(actual, expected) checkNarrow((actual), (expected), __FILE__, __LINE__)

using MoqiPanel::convertPunctuation;

// 用户定稿的映射表（2026-11-26，11-27 追加五个），逐条锁定——改表必须同时改这里。
void testPunctuation() {
  CHECK_WIDE(convertPunctuation(L"，"), L",");
  CHECK_WIDE(convertPunctuation(L"。"), L".");
  CHECK_WIDE(convertPunctuation(L"、"), L",");   // 顿号并入逗号
  CHECK_WIDE(convertPunctuation(L"；"), L";");
  CHECK_WIDE(convertPunctuation(L"："), L":");
  CHECK_WIDE(convertPunctuation(L"？"), L"?");
  CHECK_WIDE(convertPunctuation(L"！"), L"!");
  CHECK_WIDE(convertPunctuation(L"（）"), L"()");
  CHECK_WIDE(convertPunctuation(L"“”"), L"\"\"");
  CHECK_WIDE(convertPunctuation(L"‘’"), L"''");
  CHECK_WIDE(convertPunctuation(L"……"), L"...");
  CHECK_WIDE(convertPunctuation(L"——"), L"-");
  CHECK_WIDE(convertPunctuation(L"～"), L"~");
  CHECK_WIDE(convertPunctuation(L"《墨奇输入板》"), L"\"墨奇输入板\"");
  // 11-27：引擎中文状态下由 $ [ ] { } 转出来的五个，按按键还原。
  CHECK_WIDE(convertPunctuation(L"\u00A5"), L"$");  // ¥（U+00A5）
  CHECK_WIDE(convertPunctuation(L"【一】"), L"[一]");
  CHECK_WIDE(convertPunctuation(L"「二」"), L"{二}");
  CHECK_WIDE(convertPunctuation(L"共\u00A5100【含税】"), L"共$100[含税]");

  // 整句；两字节规则优先，而且只认两字节：单独的 … — 是英文里的正当标点，不转。
  CHECK_WIDE(convertPunctuation(L"你好，世界。"), L"你好,世界.");
  CHECK_WIDE(convertPunctuation(L"等等……还有——这个"), L"等等...还有-这个");
  CHECK_WIDE(convertPunctuation(L"today—perfect… wait"), L"today—perfect… wait");
  CHECK_WIDE(convertPunctuation(L"a…b—c"), L"a…b—c");
  CHECK_WIDE(convertPunctuation(L"…——…"), L"…-…");   // 三个连排：中间的成对，两边的保留
  CHECK_WIDE(convertPunctuation(L"“引号”与‘单引号’"), L"\"引号\"与'单引号'");

  // 不在表里的一律原样：英文字母数字、英文标点、日文假名、制表符、换行。
  CHECK_WIDE(convertPunctuation(L"Hello, world! (a+b)"), L"Hello, world! (a+b)");
  CHECK_WIDE(convertPunctuation(L"a\tb\r\nc"), L"a\tb\r\nc");
  CHECK_WIDE(convertPunctuation(L"かな 〔二〕 〈三〉 『四』"), L"かな 〔二〕 〈三〉 『四』");
  CHECK_WIDE(convertPunctuation(L""), L"");
  // 半角空格与全角空格都不是标点，不动。
  CHECK_WIDE(convertPunctuation(L"a b\u3000c"), L"a b\u3000c");
}

void testContentStamp() {
  using MoqiPanel::contentStamp;
  // FNV-1a 64 的标准测试向量。
  CHECK_NARROW(contentStamp("a"), "af63dc4c8601ec8c");
  CHECK_NARROW(contentStamp("foobar"), "85944171f73967e8");
  CHECK_NARROW(contentStamp(""), "none");  // 补丁不存在与空补丁是同一个戳
  CHECK(contentStamp("ab") != contentStamp("abc"));
}

void testCountsAndTrim() {
  using MoqiPanel::codePointCount;
  using MoqiPanel::kMaxTranslateCodePoints;
  using MoqiPanel::trimmed;
  CHECK(codePointCount(L"") == 0);
  CHECK(codePointCount(L"你好") == 2);
  CHECK(codePointCount(L"a，b。") == 4);
  // 代理对算一个码点。
  CHECK(codePointCount(L"\xD83D\xDE00") == 1);
  CHECK(codePointCount(L"a\xD83D\xDE00" L"b") == 3);

  // 256 字护栏：恰好 256 放行，257 超限；首尾空白不算字。
  const std::wstring exactly(kMaxTranslateCodePoints, L'字');
  CHECK(codePointCount(exactly) == kMaxTranslateCodePoints);
  CHECK(!(codePointCount(exactly) > kMaxTranslateCodePoints));
  CHECK(codePointCount(exactly + L'字') > kMaxTranslateCodePoints);
  CHECK(codePointCount(trimmed(L"\r\n  " + exactly + L" \t")) == kMaxTranslateCodePoints);

  CHECK_WIDE(trimmed(L"  你好 \u3000\r\n"), L"你好");
  CHECK_WIDE(trimmed(L"a  b"), L"a  b");   // 只去首尾
  CHECK_WIDE(trimmed(L"\u3000\u3000"), L"");
}

void testUtf8RoundTrip() {
  const std::wstring text = L"墨奇输入板 MoqiPanel，。\xD83D\xDE00";
  CHECK_WIDE(MoqiPanel::utf8ToWide(MoqiPanel::wideToUtf8(text)), text);
  CHECK(MoqiPanel::wideToUtf8(L"").empty());
  CHECK(MoqiPanel::utf8ToWide("").empty());
}

void writeFile(const std::filesystem::path& file, const std::string& bytes) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void testConfigLoad() {
  using MoqiPanel::TranslateConfig;
  using MoqiPanel::loadTranslateConfig;
  const std::filesystem::path scratch =
      std::filesystem::current_path() / "panel_core_test_tmp";
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);
  const std::filesystem::path file = scratch / "translate.json";
  TranslateConfig config;
  std::wstring error;

  // 文件不存在 = 未配置，不是错误。
  CHECK(loadTranslateConfig(file, &config, &error));
  CHECK(!config.configured());
  CHECK(error.empty());

  // 正常文件：首尾空白被裁掉。
  writeFile(file,
            "{\n"
            "  \"base_url\": \" https://api.example.com/v1 \\n\",\n"
            "  \"api_key\": \"sk-test \",\n"
            "  \"model\": \"m1\",\n"
            "  \"proxy\": \"direct\",\n"
            "  \"max_tokens\": 512,\n"
            "  \"extra_body\": {\"thinking\": {\"type\": \"disabled\"}}\n"
            "}\n");
  CHECK(loadTranslateConfig(file, &config, &error));
  CHECK_NARROW(config.baseUrl, "https://api.example.com/v1");
  CHECK_NARROW(config.apiKey, "sk-test");
  CHECK_NARROW(config.model, "m1");
  CHECK_NARROW(config.proxy, "direct");
  CHECK(config.maxTokens == 512);
  CHECK(config.configured());
  CHECK(config.extraBody["thinking"]["type"].asString() == "disabled");

  // 记事本存出的 UTF-8 带 BOM：首键名不能被 BOM 弄坏。
  writeFile(file, "\xEF\xBB\xBF{\"base_url\":\"https://a.b\",\"model\":\"m\"}");
  CHECK(loadTranslateConfig(file, &config, &error));
  CHECK_NARROW(config.baseUrl, "https://a.b");

  // 类型写错不能抛、不能崩：字符串字段按空处理。
  writeFile(file, "{\"base_url\": 5, \"api_key\": [], \"model\": {}}");
  CHECK(loadTranslateConfig(file, &config, &error));
  CHECK(!config.configured());

  // JSON 写坏 / 外层不是对象 / extra_body 不是对象 / max_tokens 不是整数：报错不抛。
  writeFile(file, "{ not json");
  CHECK(!loadTranslateConfig(file, &config, &error));
  CHECK(!error.empty());
  writeFile(file, "[1,2,3]");
  CHECK(!loadTranslateConfig(file, &config, &error));
  writeFile(file, "{\"base_url\":\"https://a.b\",\"model\":\"m\",\"extra_body\":\"x\"}");
  CHECK(!loadTranslateConfig(file, &config, &error));
  writeFile(file, "{\"base_url\":\"https://a.b\",\"model\":\"m\",\"max_tokens\":\"many\"}");
  CHECK(!loadTranslateConfig(file, &config, &error));

  // 保存 → 回读；不留 .tmp。
  TranslateConfig saved;
  saved.baseUrl = "https://api.deepseek.com";
  saved.apiKey = "k";
  saved.model = "deepseek-flash";
  saved.proxy = "http://127.0.0.1:7890";
  saved.maxTokens = 0;
  saved.extraBody = Json::Value(Json::objectValue);
  saved.extraBody["temperature"] = 0.3;
  CHECK(MoqiPanel::saveTranslateConfig(file, saved));
  CHECK(!std::filesystem::exists(file.wstring() + L".tmp"));
  TranslateConfig reloaded;
  CHECK(loadTranslateConfig(file, &reloaded, &error));
  CHECK_NARROW(reloaded.baseUrl, saved.baseUrl);
  CHECK_NARROW(reloaded.apiKey, "k");
  CHECK_NARROW(reloaded.model, saved.model);
  CHECK_NARROW(reloaded.proxy, saved.proxy);
  CHECK(reloaded.maxTokens == 0);   // 0 = 不写进文件
  CHECK(reloaded.extraBody["temperature"].asDouble() == 0.3);

  // 模板：只在文件不存在时写，写完是合法配置。
  std::filesystem::remove(file, ec);
  MoqiPanel::ensureTranslateConfigTemplate(file);
  CHECK(std::filesystem::exists(file));
  TranslateConfig fromTemplate;
  CHECK(loadTranslateConfig(file, &fromTemplate, &error));
  CHECK(!fromTemplate.baseUrl.empty());   // 给出可直接改的样子
  CHECK_NARROW(fromTemplate.model, "deepseek-flash");  // 默认模型（deepseek-chat 已下架）

  std::filesystem::remove_all(scratch, ec);
}

void testUrlAndRequest() {
  using MoqiPanel::chatCompletionsUrl;
  // 三种写法归一。
  CHECK_NARROW(chatCompletionsUrl("https://api.deepseek.com"),
               "https://api.deepseek.com/chat/completions");
  CHECK_NARROW(chatCompletionsUrl(" https://api.deepseek.com/v1/ "),
               "https://api.deepseek.com/v1/chat/completions");
  CHECK_NARROW(chatCompletionsUrl("https://api.deepseek.com/v1/chat/completions"),
               "https://api.deepseek.com/v1/chat/completions");

  MoqiPanel::TranslateConfig config;
  config.baseUrl = "https://api.deepseek.com";
  config.model = "deepseek-flash";
  const std::string body = MoqiPanel::buildTranslateRequest(config, "你好");
  // 请求体本身必须是合法 JSON（jsoncpp 会把非 ASCII 写成 \uXXXX 转义，所以
  // 这里解析回去比在原文里找中文字节可靠）。
  Json::CharReaderBuilder readerBuilder;
  std::unique_ptr<Json::CharReader> reader(readerBuilder.newCharReader());
  Json::Value request;
  std::string parseError;
  CHECK(reader->parse(body.data(), body.data() + body.size(), &request, &parseError));
  CHECK(request.isObject());
  CHECK_NARROW(request["model"].asString(), "deepseek-flash");
  CHECK(request["messages"].isArray() && request["messages"].size() == 2);
  CHECK_NARROW(request["messages"][0]["role"].asString(), "system");
  CHECK_NARROW(request["messages"][1]["role"].asString(), "user");
  CHECK_NARROW(request["messages"][1]["content"].asString(), "你好");
  // deepseek 系模型默认显式关思考。
  CHECK(request["thinking"].isObject());
  CHECK_NARROW(request["thinking"]["type"].asString(), "disabled");
  // 默认不下发 max_tokens / temperature：推理模型对这两个参数最挑剔。
  CHECK(!request.isMember("max_tokens"));
  CHECK(!request.isMember("temperature"));
  CHECK(body.find('\n') == std::string::npos);   // 单行紧凑，日志/排错里好读

  // 非 deepseek 模型不带 thinking 字段（别家接口可能不认）。
  config.model = "gpt-x";
  const std::string other = MoqiPanel::buildTranslateRequest(config, "你好");
  Json::Value otherRequest;
  CHECK(reader->parse(other.data(), other.data() + other.size(), &otherRequest,
                      &parseError));
  CHECK(!otherRequest.isMember("thinking"));
  // 大小写不敏感；DeepSeek-X 也带。
  config.model = "DeepSeek-Flash";
  Json::Value casedRequest;
  const std::string cased = MoqiPanel::buildTranslateRequest(config, "你好");
  CHECK(reader->parse(cased.data(), cased.data() + cased.size(), &casedRequest,
                      &parseError));
  CHECK_NARROW(casedRequest["thinking"]["type"].asString(), "disabled");

  // 附加参数并入请求体，但不许顶掉 model / messages；手填的 thinking 优先。
  config.model = "deepseek-flash";
  config.maxTokens = 256;
  config.extraBody = Json::Value(Json::objectValue);
  config.extraBody["thinking"] = "off";
  config.extraBody["model"] = "hijacked";
  config.extraBody["messages"] = "hijacked";
  const std::string merged = MoqiPanel::buildTranslateRequest(config, "你好");
  Json::Value mergedRequest;
  CHECK(reader->parse(merged.data(), merged.data() + merged.size(), &mergedRequest,
                      &parseError));
  CHECK_NARROW(mergedRequest["thinking"].asString(), "off");
  CHECK(mergedRequest["max_tokens"].asInt() == 256);
  CHECK_NARROW(mergedRequest["model"].asString(), "deepseek-flash");
  CHECK_NARROW(mergedRequest["messages"][1]["content"].asString(), "你好");
  CHECK(merged.find("hijacked") == std::string::npos);

  // 自定义提示词优先于内置提示词。
  config.systemPrompt = "Translate to English only.";
  const std::string custom = MoqiPanel::buildTranslateRequest(config, "你好");
  CHECK(custom.find("Translate to English only.") != std::string::npos);
  CHECK(custom.find(MoqiPanel::defaultSystemPrompt()) == std::string::npos);

  // 非法地址在拼 URL 时不会被包装成合法地址（留给网络层报错）。
  CHECK_NARROW(chatCompletionsUrl("api.deepseek.com"), "api.deepseek.com/chat/completions");
}

void testResponseParsing() {
  std::string text;
  std::string error;
  using MoqiPanel::parseChatText;
  CHECK(parseChatText(R"({"choices":[{"message":{"content":"Hello, world."}}]})", &text, &error));
  CHECK_NARROW(text, "Hello, world.");

  // 首尾空白、整段外面包的一层引号、代码围栏：都剥掉。
  CHECK(parseChatText(
            "{\"choices\":[{\"message\":{\"content\":\"  \\n\\\"Hello\\\" \\n\"}}]}",
            &text, &error));
  CHECK_NARROW(text, "Hello");
  CHECK(parseChatText(
            "{\"choices\":[{\"message\":{\"content\":\"\xE2\x80\x9C你好，世界。\xE2\x80\x9D\"}}]}",
            &text, &error));
  CHECK_NARROW(text, "你好，世界。");
  CHECK(parseChatText(
            R"({"choices":[{"message":{"content":"```text\nHello\n```"}}]})",
            &text, &error));
  CHECK_NARROW(text, "Hello");

  // 分段 content（部分厂商的形状）。
  CHECK(parseChatText(
            R"({"choices":[{"message":{"content":[{"type":"text","text":"Hel"},{"type":"text","text":"lo"}]}}]})",
            &text, &error));
  CHECK_NARROW(text, "Hello");

  // 单独一个引号不算包装，不能把内容剥没。
  CHECK(parseChatText("{\"choices\":[{\"message\":{\"content\":\"\\\"\"}}]}", &text, &error));
  CHECK_NARROW(text, "\"");

  // 失败形状：error 对象 / 空 choices / 空 content / 非法 JSON。
  CHECK(!parseChatText(R"({"error":{"message":"Insufficient Balance"}})", &text, &error));
  CHECK_NARROW(error, "Insufficient Balance");
  CHECK(!parseChatText(R"({"choices":[]})", &text, &error));
  CHECK(!parseChatText(R"({"choices":[{"message":{"content":"  "}}]})", &text, &error));
  CHECK(!parseChatText("not json", &text, &error));

  // HTTP 错误体的可读化：三种常见形状 + 兜底截断。
  CHECK_NARROW(MoqiPanel::describeHttpError(401, R"({"error":{"message":"Invalid API key"}})"),
               "HTTP 401：Invalid API key");
  CHECK_NARROW(MoqiPanel::describeHttpError(500, R"({"message":"boom"})"), "HTTP 500：boom");
  CHECK_NARROW(MoqiPanel::describeHttpError(403, R"({"error":"forbidden"})"),
               "HTTP 403：forbidden");
  CHECK_NARROW(MoqiPanel::describeHttpError(502, "<html>bad gateway</html>"),
               "HTTP 502：<html>bad gateway</html>");
  CHECK_NARROW(MoqiPanel::describeHttpError(503, ""), "HTTP 503");
}

} // namespace

int main() {
  testPunctuation();
  testContentStamp();
  testCountsAndTrim();
  testUtf8RoundTrip();
  testConfigLoad();
  testUrlAndRequest();
  testResponseParsing();
  std::printf("panel_core_test: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
