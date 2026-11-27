// 翻译设置对话框（模态）：接口地址 / API 密钥 / 模型 / 代理 / 附加参数。
// 不依赖对话框资源模板，控件在代码里建——换高 DPI 时按当前比例算坐标。
#pragma once
#include <windows.h>

#include <string>

#include "TranslateCore.h"

namespace MoqiPanel {

// 返回 true = 用户点了保存，saved 是对话框里的值（已通过基本校验）。
// loadError 非空时显示在顶部（现有 translate.json 读不出来时，仍允许改好再保存）。
bool showSettingsDialog(HWND owner, HFONT font, const std::wstring& loadError,
                        const TranslateConfig& initial, TranslateConfig* saved);

} // namespace MoqiPanel
