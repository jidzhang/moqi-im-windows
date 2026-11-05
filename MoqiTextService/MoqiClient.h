//
//	Copyright (C) 2014 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
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

#ifndef _Moqi_CLIENT_H_
#define _Moqi_CLIENT_H_
#ifndef NDEBUG
#define NDEBUG
#endif
#include <libIME2/src/TextService.h>
#include <libIME2/src/KeyEvent.h>
#include <libIME2/src/EditSession.h>
#include "MoqiLangBarButton.h"

#include <deque>
#include <atomic>
#include <unordered_map>
#include <string>
#include <vector>
#include <json/json.h>
#include "LocalEngine.h"

namespace Moqi {

class TextService;

struct AutoPairRuleState {
	std::wstring open;
	std::wstring close;
};

class Client
{
public:
	// create a client connection to the input method server
	// guid is the GUID of the language profile
	Client(TextService* service, REFIID langProfileGuid);
	~Client(void);

	const std::string guid() const {
		return guid_;
	}

	// handlers for the text service
	void onActivate();
	void onDeactivate();

	bool filterKeyDown(Ime::KeyEvent& keyEvent);
	bool onKeyDown(Ime::KeyEvent& keyEvent, Ime::EditSession* session);

	bool filterKeyUp(Ime::KeyEvent& keyEvent);
	bool onKeyUp(Ime::KeyEvent& keyEvent, Ime::EditSession* session);

	bool onPreservedKey(const GUID& guid);
	bool onPreservedKey(const GUID& guid, Ime::EditSession* session);
	bool highlightCandidate(int index);
	bool selectCandidate(int index);
	bool changePage(bool backward);

	bool onCommand(UINT id, Ime::TextService::CommandType type);

	// called when a language bar button needs a menu
	bool onMenu(LangBarButton* btn, ITfMenu* pMenu);

	// called when a language bar button needs a menu
	HMENU onMenu(LangBarButton* btn);

	// called when a compartment value is changed
	void onCompartmentChanged(const GUID& key);

	// called when the keyboard is opened or closed
	void onKeyboardStatusChanged(bool opened);

	// called just before current composition is terminated for doing cleanup.
	void onCompositionTerminated(bool forced);

private:
    // 架构重构阶段1：热键路径优先走进程内引擎（见 23-架构重构文档）。
    // engaged 后本 Client 的按键/候选操作不再发管道 RPC；任何初始化失败
    // 都会让该进程一直停留在 RPC 回退路径。
    bool maybeEngageLocalEngine();
    static void requestLocalEngineInit(TextService* service);

    static std::wstring getPipeName(const wchar_t* base_name);
    HANDLE connectPipe(const wchar_t* pipeName, int timeoutMs);
    bool ensureLauncherRunning();
    bool isLauncherProcessRunning();
    bool forceRestartLauncher();

    Json::Value createRpcRequest(const char* methodName);
	bool callRpcMethod(Json::Value& request, Json::Value& response);
    void sendRpcNoWait(const char* methodName);
    // 可靠版（仅后台线程）：陈旧管道重连一次再发。TSF 线程禁用——重连会
    // 阻塞（用户词典合并通知用）。
    void sendRpcNoWaitReliable(const char* methodName);

    bool isPipeCreatedByMoqiServer(HANDLE pipe);
    bool waitForRpcConnection(int connectAttempts);
    bool completeConnectionHandshake();
    bool callRpcPipe(HANDLE pipe, const std::string& serializedRequest, std::string& serializedReply, bool* timedOut = nullptr);
    bool writePipeMessageWithTimeout(HANDLE pipe, const char* data, size_t len, DWORD timeoutMs);
    bool readPipeMessageWithTimeout(HANDLE pipe, std::string& message, DWORD timeoutMs, bool* timedOut = nullptr);
	bool waitForRpcIdle(int timeoutMs) const;
	bool waitForRpcIdleForFirstKey() const;
	bool readPendingPipeMessage(std::string& serializedReply);
	void refreshAsyncPollTimer();
	void pollAsyncResponses();

public:
  // 候选窗消息泵里的轮询入口（WM_TIMER 转发；异步推送取回用）。
  void pollAsyncForUi() { pollAsyncResponses(); }

  // 用户词典学习导出（2026-11-02）：安全时机（无组字）后台导出本进程
  // userdb 快照→拷主词典 sync/→通知 daemon 合并。触发点=真反激活/提交后/
  // 切走焦点；20 秒防抖 + 在途互斥。requireDirty=false 供 onDeactivate
  //（脏标记已被消费）。force=true 跳过防抖：反激活是进程退出前最后一次
  // 机会，被防抖吞掉就等于丢字（2026-11-03 用户实报"重启记事本就忘了"）。
  void scheduleUserDbExport(bool checkComposition, bool requireDirty = true,
                            bool force = false);

private:
    void enqueueAsyncResponse(const Json::Value& msg);
	void flushPendingAsyncResponsesWithCurrentContext();
	void flushPendingAsyncResponses(Ime::EditSession* session = nullptr);
	bool applyAsyncResponse(Json::Value& msg, Ime::EditSession* session = nullptr);
	static void CALLBACK onAsyncPollTimer(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
    void closeRpcConnection();

	bool init();
    void resetTextServiceState();

    void addKeyEventToRpcRequest(Json::Value& request, Ime::KeyEvent& keyEvent);
    bool sendOnMenu(std::string button_id, Json::Value& result);

    // applyEngineState=false：辅助方法（init/onActivate/onCommand/onMenu/
    // compartment/keyboard）的响应只带控制回执，零值状态字段必须忽略。
    bool handleRpcResponse(Json::Value& msg, Ime::EditSession* session, bool applyEngineState);

    // Update text service and UI status based on RPC responses.
    void updateSelectionKeys(Json::Value& msg);
    void updateMessageWindow(Json::Value& msg, Ime::EditSession* session, bool& endComposition);
	void updateCommitString(Json::Value& msg, Ime::EditSession* session, bool suppressTerminationNotification);
	void updateComposition(Json::Value& msg, Ime::EditSession* session, bool& endComposition);
	void updateLanguageButtons(Json::Value& msg);
	void updatePreservedKeys(Json::Value& msg);
	void updateKeyboardStatus(Json::Value& msg);
    void updateCandidateList(Json::Value& msg, Ime::EditSession* session);
    void updateUI(const Json::Value& data);
    void updateStatus(Json::Value& msg, Ime::EditSession* session, bool applyEngineState);

private:
	TextService* textService_;
	std::string guid_;
	HANDLE pipe_;
	std::atomic<int> rpcInProgress_;
	std::atomic<bool> activationInProgress_;
	unsigned int nextSeqNum_;
	bool isActivated_;
    bool shouldWaitConnection_;
    bool handshakeComplete_;
    int connectAttempts_;
    ULONGLONG lastLauncherStartAttemptTick_;
    ULONGLONG lastLauncherKillTick_;
	HWND asyncPollTimerWindow_;
	UINT_PTR asyncPollTimerId_;
	bool asyncFlushInProgress_;
	std::deque<Json::Value> pendingAsyncResponses_;
	std::vector<AutoPairRuleState> autoPairRules_;
	LocalEngine localEngine_;
	bool keyboardHealAttempted_;
	// 云剪贴板列表激活期间（daemon 推送帧带 cloudClip 标记时置 true），
	// 所有按键走 RPC（本地引擎不处理——否则 rime 吃掉选字键刷掉列表）。
	bool cloudClipRpcMode_ = false;
	// 最近一次语言栏菜单树（onMenu 响应）。onCommand 的开关区命令（2000+）
	// 据此把 id 映射回选项名，在本地引擎上直接翻转（daemon 在树里带
	// "option" 字段）。
	Json::Value lastMenuItems_{Json::arrayValue};
	std::atomic<bool> userDbExportInFlight_{false};
	// 最近一次代插的提交文本是否为单个数字（数字直通）。数字后紧跟的 "."
	// 直接放行给应用原生插入：Word 2016 的自动编号窗口期会丢掉紧随数字
	// 提交之后的第二次 IME 代插（2026-11-05 用户实报，微软/谷歌为键盘
	// 放行所以免疫；记事本/WPS 宽容不受影响）。
	bool lastCommitWasDigit_ = false;
	// filterKeyDown 已放行 "." 给应用后置位；TSF 的 OnKeyDown 阶段对同一
	// 键必须继续返回"不吃"，否则 daemon 的 lastKeyDownRet_ 残留会把句点
	// 重新吃掉（放行两阶段必须一致）。
	bool digitPeriodPassthroughPending_ = false;
	ULONGLONG lastUserDbExportTick_ = 0;
};

}

#endif // _Moqi_CLIENT_H_
