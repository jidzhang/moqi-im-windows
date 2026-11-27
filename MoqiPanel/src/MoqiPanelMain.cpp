// 墨奇输入板入口。
#include <windows.h>

#include <commctrl.h>
#include <imm.h>

#include <string>

#include "PanelWindow.h"

using MoqiPanel::PanelWindow;

namespace {

std::wstring executableDir() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  const std::wstring full(path, length);
  const size_t slash = full.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
  // 本进程彻底关掉系统输入法（含墨奇自己的 TSF DLL）：面板的按键只进自带
  // 引擎。必须先于任何窗口创建。
  ::ImmDisableIME(static_cast<DWORD>(-1));
  INITCOMMONCONTROLSEX controls = {};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_STANDARD_CLASSES;
  ::InitCommonControlsEx(&controls);

  if (PanelWindow::registerClass(instance) == 0) return 1;
  std::wstring why;
  PanelWindow* panel = PanelWindow::create(instance, executableDir(), &why);
  if (panel == nullptr) {
    if (!why.empty()) {
      ::MessageBoxW(nullptr, why.c_str(), L"Moqi Panel - Cannot Start", MB_OK | MB_ICONERROR);
    }
    return 2;
  }
  ::ShowWindow(panel->hwnd(), showCmd);
  ::UpdateWindow(panel->hwnd());

  MSG msg = {};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // 按键闸门：输入区的键盘消息先不翻译成字符——引擎说"没吃"时由面板
    // 用扣住那一刻的键盘状态补做 TranslateMessage（见 PanelActions.cpp）。
    if (!panel->holdKeyMessage(msg)) {
      ::TranslateMessage(&msg);
    }
    ::DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}
