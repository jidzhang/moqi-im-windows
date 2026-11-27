#pragma once
// 面板纯逻辑（不依赖窗口/控件；单元测试 tests/panel_core_test.cpp 直接链接）。
#include <cstddef>
#include <string>

namespace MoqiPanel {

// 进剪贴板文本的唯一出口：中文/全角标点 → 英文/半角。
// 四条复制路径（翻译并复制、复制原文、重新复制、选中自动复制）都必须经过它，
// 映射规则只写在这一处——以后新增复制路径不会漏。
std::wstring convertPunctuation(const std::wstring& text);

std::wstring utf8ToWide(const std::string& utf8);
std::string wideToUtf8(const std::wstring& wide);

// Unicode 码点数（代理对算 1 个）。翻译字数护栏与界面计数共用。
size_t codePointCount(const std::wstring& text);

// 翻译上限（用户定：256 字）。
constexpr size_t kMaxTranslateCodePoints = 256;

// 去掉首尾空白（含全角空格）。字数按它算，发往翻译接口的也是它。
std::wstring trimmed(const std::wstring& text);

// 内容戳（64 位 FNV-1a 的十六进制；空内容给 "none"）。面板用它记住"上次部署时
// 自带的数据补丁长什么样"，补丁变了就清 user\build 让引擎重新部署（EngineClient）。
std::string contentStamp(const std::string& bytes);

} // namespace MoqiPanel
