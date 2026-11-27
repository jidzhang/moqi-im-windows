// include 纪律：windows.h 开篇（与 daemon 同，见 DaemonAI.cpp 头注释）。
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

#include "PanelCore.h"

namespace MoqiPanel {
namespace {

struct PunctRule {
  const wchar_t* from;
  const wchar_t* to;
};

// 严格按用户定稿的映射表（2026-11-26），不多不少。
// 两字节的规则排在前面：先试"……""——"，再落到单字规则。
// 《》→ 英文双引号、顿号并入逗号、破折号转连字符。
// 单独的"…"和"—"不在表里、也不转：它们在英文译文里是正当的省略号和破折号
//（模型很爱在英文里用"today—perfect"），换掉反而成了毛病。
// 2026-11-27 追加五个（用户："我要纯英文的"）：引擎在中文状态下会把
// $ [ ] { } 上屏成 ¥ 【 】 「 」，按按键还原。
constexpr PunctRule kPunctRules[] = {
    {L"……", L"..."}, {L"——", L"-"},
    {L"，", L","},   {L"。", L"."},   {L"、", L","},   {L"；", L";"},
    {L"：", L":"},   {L"？", L"?"},   {L"！", L"!"},   {L"（", L"("},
    {L"）", L")"},   {L"“", L"\""},  {L"”", L"\""},  {L"‘", L"'"},
    {L"’", L"'"},    {L"～", L"~"},
    {L"《", L"\""},  {L"》", L"\""},
    {L"\u00A5", L"$"}, {L"【", L"["}, {L"】", L"]"},  {L"「", L"{"},
    {L"」", L"}"},
};

bool isOuterSpace(wchar_t c) {
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
         c == L'\u3000' ||  // 全角空格
         c == L'\u00A0' ||  // 不换行空格
         c == L'\uFEFF';    // BOM
}

} // namespace

std::wstring convertPunctuation(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    bool matched = false;
    for (const PunctRule& rule : kPunctRules) {
      const size_t n = std::wcslen(rule.from);
      if (text.compare(i, n, rule.from) == 0) {
        out += rule.to;
        i += n;
        matched = true;
        break;
      }
    }
    if (!matched) {
      out += text[i++];
    }
  }
  return out;
}

std::wstring utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                        static_cast<int>(utf8.size()), &out[0], n);
  return out;
}

std::string wideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                      static_cast<int>(wide.size()), nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                        static_cast<int>(wide.size()), &out[0], n, nullptr,
                        nullptr);
  return out;
}

size_t codePointCount(const std::wstring& text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    // 低代理不单独计数（它与前一个高代理合成一个码点）。
    const bool lowSurrogate = c >= 0xDC00 && c <= 0xDFFF;
    if (!lowSurrogate) ++count;
  }
  return count;
}

std::wstring trimmed(const std::wstring& text) {
  size_t b = 0;
  size_t e = text.size();
  while (b < e && isOuterSpace(text[b])) ++b;
  while (e > b && isOuterSpace(text[e - 1])) --e;
  return text.substr(b, e - b);
}

std::string contentStamp(const std::string& bytes) {
  if (bytes.empty()) return "none";
  uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 0x100000001b3ull;  // FNV prime
  }
  char buf[20] = {};
  snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

} // namespace MoqiPanel
