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

#ifndef Moqi_LANGUAGE_BAR_BUTTON_H
#define Moqi_LANGUAGE_BAR_BUTTON_H

#include <libIME2/src/LangBarButton.h>
#include <json/json.h>
#include <string>

namespace Moqi {

class TextService;

class LangBarButton : public Ime::LangBarButton {
public:
	LangBarButton(TextService* service, const std::string& id, const GUID& guid, UINT commandId = 0, const wchar_t* text = NULL, DWORD style = TF_LBI_STYLE_BTN_BUTTON);

	static LangBarButton* fromJson(TextService* service, const Json::Value& info);
	void updateFromJson(const Json::Value& info);

	const std::string& id() const {
		return id_;
	}

    void setIconFile(std::wstring filePath);

	// ITfLangBarItemButton
	STDMETHODIMP GetInfo(TF_LANGBARITEMINFO *pInfo);
	STDMETHODIMP GetStatus(DWORD *pdwStatus);
	STDMETHODIMP GetIcon(HICON *phIcon);
	STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT *prcArea);
	STDMETHODIMP InitMenu(ITfMenu *pMenu);

protected:
	virtual ~LangBarButton(); // COM object should be deleted using Rlease()

private:
	std::string id_;
    std::wstring iconFile_;
	// 图标由按钮自持（bug#21，2026-11-05）：旧实现是进程级 static 缓存 +
	// Client 反激活时 clearIconCache() 把缓存里的 HICON 全 DestroyIcon。同进程
	// 其它线程仍在用的按钮持有同一批句柄，销毁后 GetIcon→CopyImage 失败返回
	// NULL，语言栏上就是"中英图标不见了/只有一边显示"，且 setIconFile 因路径
	// 未变而永不重载，进程内一击不可逆。改为每个按钮 LoadImage 一份、随按钮
	// 析构释放（图标文件很小，去重收益不值这个坑）。
	HICON ownedIcon_ = nullptr;
	// 诊断（bug#21 现场取证）：msctf 每次问 GetInfo/GetStatus 的结论，内容
	// 变化时立刻记，否则每 kInfoHeartbeatMs 记一次心跳（判断"图标消失后
	// msctf 还问不问我们"）。
	static constexpr ULONGLONG kInfoHeartbeatMs = 30000;
	std::wstring lastGetInfoLine_;
	std::wstring lastGetStatusLine_;
	ULONGLONG lastGetInfoTick_ = 0;
	ULONGLONG lastGetStatusTick_ = 0;
	ULONGLONG lastGetIconTick_ = 0;
	HRESULT lastGetIconOk_ = S_OK;
};

} // namespace Moqi

#endif // Moqi_LANGUAGE_BAR_BUTTON_H
