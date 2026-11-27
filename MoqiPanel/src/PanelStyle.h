#pragma once
// 主窗口的样式常量与几个小工具（PanelWindow.cpp / PanelActions.cpp 共用）。
#include <windows.h>

#include <string>

namespace MoqiPanel {

constexpr wchar_t kClassName[] = L"MoqiPanelWindow";
constexpr wchar_t kWindowTitle[] = L"墨奇输入板";

// 候选高亮色与墨奇候选窗同色（MoqiTextService 的 candHighlightColor_）。
constexpr COLORREF kWindowBg = RGB(245, 246, 248);
constexpr COLORREF kResultBg = RGB(248, 249, 250);
constexpr COLORREF kSeparator = RGB(224, 227, 231);
constexpr COLORREF kText = RGB(24, 24, 24);
constexpr COLORREF kMutedText = RGB(128, 132, 138);
constexpr COLORREF kHighlight = RGB(198, 221, 249);
constexpr COLORREF kHighlightBorder = RGB(122, 172, 232);
constexpr COLORREF kAccent = RGB(0, 116, 190);
constexpr COLORREF kStatusNeutral = RGB(96, 100, 106);
constexpr COLORREF kStatusSuccess = RGB(16, 124, 16);
constexpr COLORREF kStatusWarn = RGB(160, 90, 0);
constexpr COLORREF kStatusError = RGB(196, 43, 28);

inline bool isKeyMessage(UINT message) {
  return message == WM_KEYDOWN || message == WM_KEYUP ||
         message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
}

// 编辑框里靠这些键移动光标/选区。
inline bool isNavigationKey(UINT vk) {
  return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
         vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT;
}

// 组字光标在回包里是"第几个码点"，画到屏幕上要换成 UTF-16 下标。
inline int utf16IndexFromCodePoints(const std::wstring& text, int codePoints) {
  const int size = static_cast<int>(text.size());
  int index = 0;
  for (int counted = 0; index < size && counted < codePoints; ++index) {
    const wchar_t c = text[index];
    const bool highSurrogate = c >= 0xD800 && c <= 0xDBFF;
    if (highSurrogate && index + 1 < size) ++index;
    ++counted;
  }
  return index;
}

// 编辑框只认 CRLF 换行；单独的 LF/CR 会显示成方块。
inline std::wstring withCrLf(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size() + 4);
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (c == L'\r') {
      if (i + 1 >= text.size() || text[i + 1] != L'\n') out += L"\r\n";
    } else if (c == L'\n') {
      out += L"\r\n";
    } else {
      out += c;
    }
  }
  return out;
}

} // namespace MoqiPanel
