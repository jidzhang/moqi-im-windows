//
//	Copyright (C) 2016 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
//
//	This library is free software; you can redistribute it and/or
//	modify it under the terms of the GNU Library General Public
//	License as published by the Free Software Foundation; either
//	version 2 of the License, or (at your option) any later version.
//
//	This library is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//	Library General Public License for more details.
//
//	You should have received a copy of the GNU Library General Public
//	License along with this library; if not, write to the
//	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//	Boston, MA  02110-1301, USA.
//


#include "MoqiLangBarButton.h"
#include "MoqiTextService.h"
#include "JsonCompat.h"
#include "KeyGuardLog.h"
#include "libIME2/src/Utils.h"

// this is the GUID of the IME mode icon in Windows 8
// the value is not available in older SDK versions, so let's define it ourselves.
static const GUID _GUID_LBI_INPUTMODE =
{ 0x2C77A81E, 0x41CC, 0x4178,{ 0xA3, 0xA7, 0x5F, 0x8A, 0x98, 0x75, 0x68, 0xE6 } };

static const char WINDOWS_MODE_ICON_ID[] = "windows-mode-icon";

namespace Moqi {

LangBarButton::LangBarButton(TextService* service, const std::string& id, const GUID& guid, UINT commandId, const wchar_t* text, DWORD style):
	Ime::LangBarButton(service, guid, commandId, text, style),
	id_(id) {
}

LangBarButton::~LangBarButton() {
	// 图标归本按钮所有（GetIcon 交给语言栏的是 CopyImage 副本），随对象释放。
	if (ownedIcon_ != nullptr) {
		::DestroyIcon(ownedIcon_);
		ownedIcon_ = nullptr;
	}
}

LangBarButton* LangBarButton::fromJson(TextService* service, const Json::Value& info) {
	if (info.isObject()) {
		std::string id = info["id"].asString();

		DWORD style = TF_LBI_STYLE_BTN_BUTTON;
		const Json::Value& styleValue = info["style"];
		if (styleValue.isUInt())
			style = styleValue.asUInt();

		CLSID guid = { 0 }; // create a new GUID on-the-fly
		if (id == WINDOWS_MODE_ICON_ID) {
			// Windows 8 systray IME mode icon
			guid = _GUID_LBI_INPUTMODE;
		}
		else {
			CoCreateGuid(&guid);
		}

		LangBarButton* langBtn = new LangBarButton(service, id, guid, 0, NULL, style);
		if (langBtn != nullptr) {
			langBtn->updateFromJson(info);
			return langBtn;
		}
	}
	return nullptr;
}

void LangBarButton::setIconFile(std::wstring filePath) {
	// 路径相同也要在"当前没有图标"时重试：加载失败一次之后必须能恢复。
	if (filePath == iconFile_ && icon() != nullptr) {
		return;
	}
	iconFile_ = std::move(filePath);

	HICON icon = (HICON)LoadImageW(NULL, iconFile_.c_str(), IMAGE_ICON, 0, 0,
	                               LR_DEFAULTCOLOR | LR_LOADFROMFILE);
	if (icon == nullptr) {
		// 保留旧图标：语言栏上宁可显示上一张（可能状态差一档），也不留空按钮。
		appendRpcGuardLog(L"[icon] load failed id=" + utf8ToUtf16(id_.c_str()) +
		                 L" path=" + iconFile_ + L" err=" +
		                 std::to_wstring(::GetLastError()));
		return;
	}
	if (ownedIcon_ != nullptr) {
		::DestroyIcon(ownedIcon_);
	}
	ownedIcon_ = icon;
	setIcon(icon);
}

void LangBarButton::updateFromJson(const Json::Value& info) {
	// 2026-11-02: 零值=未指定，不更新（后端 protojson EmitUnpopulated 会把
	// changeButton 的未设置字段以零值下发）。解析规则见 JsonCompat.h，
	// 单测见 tests/protocol_json_test.cpp。
	const Moqi::ProtocolJson::ButtonPatch patch =
		Moqi::ProtocolJson::parseButtonPatch(info);
	if (patch.hasIcon) {
		setIconFile(utf8ToUtf16(patch.icon.c_str()));
	}
	if (patch.hasCommandId) {
		setCommandId(patch.commandId);
	}
	if (patch.hasText) {
		const std::wstring text = utf8ToUtf16(patch.text.c_str());
		setText(text.c_str());
	}
	if (patch.hasTooltip) {
		const std::wstring tooltip = utf8ToUtf16(patch.tooltip.c_str());
		setTooltip(tooltip.c_str());
	}
	if (patch.kind != Moqi::ProtocolJson::ButtonPatch::Kind::None) {
		DWORD newStyle = style() & ~(TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_MENU | TF_LBI_STYLE_BTN_TOGGLE);
		switch (patch.kind) {
		case Moqi::ProtocolJson::ButtonPatch::Kind::Button:
			newStyle |= TF_LBI_STYLE_BTN_BUTTON;
			break;
		case Moqi::ProtocolJson::ButtonPatch::Kind::Toggle:
			newStyle |= TF_LBI_STYLE_BTN_TOGGLE;
			break;
		case Moqi::ProtocolJson::ButtonPatch::Kind::Menu:
			newStyle |= TF_LBI_STYLE_BTN_MENU;
			break;
		default:
			break;
		}
		setStyle(newStyle);
	}
	if (patch.hasEnable) {
		setEnabled(patch.enable);
	}
	if (patch.hasToggled) {
		setToggled(patch.toggled);
	}
}


// 语言栏每次重绘都会来取图标。这里为空说明图标没加载成功（或句柄已失效），
// 记一条无条件日志——现场"中英图标不见了"时靠它区分是图标问题还是栏项丢失。
// 成功路径只在 trace 档记心跳（bug#21 现场取证口径：图标消失后 GetIcon
// 还来不来是关键证据），避免默认 error 档被常规重绘刷屏（用户 2026-11-05
// 反馈"默认 error 都会生成很多日志，不受控"）。
STDMETHODIMP LangBarButton::GetIcon(HICON *phIcon) {
	const ULONGLONG now = ::GetTickCount64();
	if (icon() == nullptr) {
		appendRpcGuardLog(L"[icon] GetIcon with no icon id=" +
		                  utf8ToUtf16(id_.c_str()) + L" path=" + iconFile_);
		*phIcon = nullptr;
		return E_FAIL;
	}
	const HRESULT hr = Ime::LangBarButton::GetIcon(phIcon);
	if (now - lastGetIconTick_ >= kInfoHeartbeatMs ||
	    (hr != S_OK) != (lastGetIconOk_ != S_OK)) {
		lastGetIconTick_ = now;
		lastGetIconOk_ = hr;
		appendRpcTraceLog(L"[icon] GetIcon id=" + utf8ToUtf16(id_.c_str()) +
		                  L" hr=" + std::to_wstring(static_cast<long>(hr)) +
		                  L" hicon_null=" + (phIcon && *phIcon ? L"0" : L"1"));
	}
	return hr;
}

// msctf 会反复来问"你是谁、什么状态"。现场"图标不显示"时，这两条是
// 定位证据（切焦点后不再被问=按钮掉了；仍被问=msctf 在画但不显示）。
// 全部走 trace 档：常规重绘频率高，无条件写会淹没 key-guard（用户反馈）。
STDMETHODIMP LangBarButton::GetInfo(TF_LANGBARITEMINFO *pInfo) {
	const HRESULT hr = Ime::LangBarButton::GetInfo(pInfo);
	if (pInfo != nullptr) {
		// 记录 guidItem 前 8 位、样式位、描述长度。样式位被洗成 0 会导致
		// 按钮不显示；desc 为空会让 Windows 10 暂时隐藏按钮。
		wchar_t guidText[40] = {};
		swprintf_s(guidText, L"{%08lX}", static_cast<unsigned long>(pInfo->guidItem.Data1));
		wchar_t styleText[16] = {};
		swprintf_s(styleText, L"0x%lx", static_cast<unsigned long>(pInfo->dwStyle));
		const std::wstring line = L"[icon] GetInfo id=" +
		                          utf8ToUtf16(id_.c_str()) + L" guid=" + guidText +
		                          L" style=" + styleText +
		                          L" desc_len=" + std::to_wstring(wcslen(pInfo->szDescription)) +
		                          L" hr=" + std::to_wstring(static_cast<long>(hr));
		const ULONGLONG now = ::GetTickCount64();
		if (line != lastGetInfoLine_ || now - lastGetInfoTick_ >= kInfoHeartbeatMs) {
			lastGetInfoLine_ = line;
			lastGetInfoTick_ = now;
			appendRpcTraceLog(line);
		}
	}
	return hr;
}

STDMETHODIMP LangBarButton::GetStatus(DWORD *pdwStatus) {
	const HRESULT hr = Ime::LangBarButton::GetStatus(pdwStatus);
	if (pdwStatus != nullptr) {
		wchar_t statusText[16] = {};
		swprintf_s(statusText, L"0x%lx", static_cast<unsigned long>(*pdwStatus));
		const std::wstring line = L"[icon] GetStatus id=" +
		                          utf8ToUtf16(id_.c_str()) + L" status=" + statusText;
		const ULONGLONG now = ::GetTickCount64();
		if (line != lastGetStatusLine_ || now - lastGetStatusTick_ >= kInfoHeartbeatMs) {
			lastGetStatusLine_ = line;
			lastGetStatusTick_ = now;
			appendRpcTraceLog(line);
		}
	}
	return hr;
}

STDMETHODIMP LangBarButton::OnClick(TfLBIClick click, POINT pt, const RECT *prcArea) {
	// special handling for right click on windows 8 mode icon
	if (id_ == WINDOWS_MODE_ICON_ID && click == TF_LBI_CLK_RIGHT) {
		TextService* service = static_cast<TextService*>(textService());
		// check if we need to show a popup menu
		HMENU popupMenu = service->onMenu(this);
		if (popupMenu != NULL) {
			Ime::Window window; // TrackPopupMenu requires a window to work, so let's build a transient one.
			window.create(HWND_DESKTOP, 0);
			// 2026-11-01 修复：不设前台 + 无 TPM_RIGHTBUTTON 时，发起右键的
			// 释放事件会立刻选中光标下的第一个菜单项（表现为"右键=中英切换、
			// 菜单一闪而过"）。MSDN 通知图标菜单配方：置前台 + RIGHTBUTTON +
			// 结束后 PostMessage(WM_NULL)。
			::SetForegroundWindow(window.hwnd());
			UINT ret = ::TrackPopupMenu(popupMenu,
			                            TPM_NONOTIFY | TPM_RETURNCMD |
			                                TPM_LEFTALIGN | TPM_BOTTOMALIGN |
			                                TPM_RIGHTBUTTON,
			                            pt.x, pt.y, 0, window.hwnd(), NULL);
			::PostMessage(window.hwnd(), WM_NULL, 0, 0);
			if (ret > 0)
				service->onCommand(ret, TextService::COMMAND_MENU);
			::DestroyMenu(popupMenu);
			return S_OK;
		}
	}
	return Ime::LangBarButton::OnClick(click, pt, prcArea);
}

STDMETHODIMP LangBarButton::InitMenu(ITfMenu *pMenu) {
	TextService* service = static_cast<TextService*>(textService());
	if (service && service->onMenu(this, pMenu)) {
		return S_OK;
	}
	return Ime::LangBarButton::InitMenu(pMenu);
}

} // namespace Moqi
