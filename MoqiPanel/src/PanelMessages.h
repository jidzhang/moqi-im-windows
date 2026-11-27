#pragma once
// 面板内部消息：工作线程 → 界面线程。参数一律是 new 出来的对象，由接收方 delete。
#include <windows.h>

namespace MoqiPanel {

constexpr UINT WM_APP_ENGINE_STATUS = WM_APP + 1;   // wParam = EngineStatus
constexpr UINT WM_APP_ENGINE_RESULT = WM_APP + 2;   // lParam = EngineResult*
constexpr UINT WM_APP_TRANSLATE_DONE = WM_APP + 3;  // lParam = TranslateOutcome*

} // namespace MoqiPanel
