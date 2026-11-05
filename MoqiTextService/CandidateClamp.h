#pragma once
// 候选窗锚点夹取的纯几何部分。Win32 部分（选显示器/取窗口尺寸/日志）
// 在 MoqiTextService.cpp 的 clampCandidateWindowAnchor 包装函数里。
#include <windows.h>

namespace Moqi {

// 把候选窗锚点（输入行左下角）夹取到工作区内：
// - 左/上越界 → 拉回工作区左/上边缘（margin 间距）；
// - 右越界 → 左移到右缘内；宽度超过工作区宽时钉在左缘；
// - 底部放不下整窗 → 优先翻到输入行上方，仍放不下才钉在工作区底部。
// work=目标显示器工作区；textRect=输入行矩形（供"翻上方"用）；
// width/height=候选窗尺寸（0=未知，跳过对应轴的夹取）。
inline void clampCandidateWindowPos(POINT* pos, const RECT& work,
                                    const RECT& textRect, int width,
                                    int height) {
	constexpr int margin = 8;
	if (pos->x < work.left + margin) {
		pos->x = work.left + margin;
	}
	if (width > 0 && pos->x + width > work.right - margin) {
		pos->x = work.right - margin - width;
		if (pos->x < work.left + margin) {
			pos->x = work.left + margin;
		}
	}
	if (pos->y < work.top + margin) {
		pos->y = work.top + margin;
	}
	if (height > 0 && pos->y + height > work.bottom - margin) {
		const int above = textRect.top - margin - height;
		if (above >= work.top + margin) {
			pos->y = above;
		} else {
			pos->y = work.bottom - margin - height;
			if (pos->y < work.top + margin) {
				pos->y = work.top + margin;
			}
		}
	}
}

} // namespace Moqi
