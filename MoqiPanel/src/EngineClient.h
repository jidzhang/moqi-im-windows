#pragma once
// 面板自带的输入引擎客户端：拉起 MoqiDaemon.exe --serve --portable，经命名管道
// 逐键对话。协议与 TSF DLL / 产品栈评测器完全一致（4 字节小端长度前缀 +
// JSON；FILTER_KEY_DOWN → 吃了才发 ON_KEY_DOWN，键松同理），所以按键语义、
// 候选排序、学习、容错、gram 都是发货链的原样行为。
// include 纪律：windows.h 开篇（见 PanelCore.cpp）。
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include "ProtoFraming.h"

namespace MoqiPanel {

// 一次物理按键事件，由编辑框收到的 WM_KEYDOWN/WM_KEYUP 原样换算。
struct KeyInput {
  UINT vk = 0;
  UINT scanCode = 0;
  bool isExtended = false;
  bool isUp = false;
  UINT charCode = 0;  // ToUnicodeEx 结果（Ctrl/Alt 清零），与 libIME2 同口径
  BYTE states[256] = {};
};

enum class EngineStatus { Connecting, Ready, Failed };

// 一次任务的结果。keyId 非零 = 按键任务，eaten 表示引擎是否吃掉了这个键；
// frames 是这条任务各 RPC 的应答（按序，界面逐帧套用）。
struct EngineResult {
  uint64_t keyId = 0;
  bool ok = false;
  bool eaten = false;
  bool isFlush = false;    // 这是"组字冲刷"任务
  bool flushed = false;    // 冲刷成功
  std::vector<Json::Value> frames;
};

class EngineClient {
public:
  EngineClient() = default;
  ~EngineClient() = default;
  EngineClient(const EngineClient&) = delete;
  EngineClient& operator=(const EngineClient&) = delete;

  // 起后台线程：拉起 daemon、连管道、初始化会话。状态与回包经 notify 窗口
  // 投递（WM_APP_ENGINE_STATUS / WM_APP_ENGINE_RESULT）。
  void start(HWND notify, const std::wstring& exeDir);

  // 请求退出：ON_DEACTIVATE → 关管道 → 线程收尾（daemon 随末连接断开自退）。
  // 最多等 waitMs；返回线程是否已经结束。没结束也不能 delete 本对象。
  bool stop(DWORD waitMs);

  void postKey(const KeyInput& key, uint64_t keyId);
  void postSelectCandidate(int index);
  void postChangePage(bool backward);
  // 合成一次 Shift 单击：与真按 Shift 切中英走同一条引擎路径。
  void postShiftTap();
  // 把当前组字用空格顶屏（翻译前不丢未上屏的拼音）。
  void postFlush();

private:
  enum class TaskKind { Key, SelectCandidate, ChangePage, ShiftTap, Flush };
  struct Task {
    TaskKind kind = TaskKind::Key;
    KeyInput key;
    uint64_t keyId = 0;
    int index = 0;
    bool backward = false;
  };

  void threadMain();
  bool connectOnce();
  bool tryOpenPipe();
  bool spawnDaemon();
  bool daemonAlive() const;
  // 自带数据补丁变了（升级包 / 被删）就把部署状态退回未部署，逼引擎重新部署。
  void refreshDataPatch();
  std::wstring buildChildEnvironment() const;
  void closePipe();

  // 一次请求→应答。应答前先到的帧（异步推送）收进 asyncOut。
  bool rpc(const std::string& method, const Json::Value& params, DWORD timeoutMs,
           Json::Value* reply, std::vector<Json::Value>* asyncOut);
  bool writeAll(const std::string& data);
  bool readFrame(DWORD timeoutMs, std::string* payload);
  bool runTask(const Task& task);
  void postResult(std::unique_ptr<EngineResult> result);
  void setStatus(EngineStatus status);
  bool popTask(Task* task, DWORD waitMs);
  void pushTask(Task task);

  HWND notify_ = nullptr;
  std::wstring exeDir_;
  std::wstring pipeName_;
  std::wstring mutexName_;
  std::thread thread_;
  std::mutex mutex_;               // 保护 queue_ 与 stopRequested_
  std::deque<Task> queue_;
  HANDLE wake_ = nullptr;          // 自动复位：有新任务/要退出时唤醒工作线程
  HANDLE done_ = nullptr;          // 工作线程收尾完成
  bool stopRequested_ = false;     // 持有 mutex_ 读写
  std::atomic<HANDLE> pipe_{nullptr};       // 工作线程写；stop() 读它来取消 I/O
  std::atomic<HANDLE> daemonProcess_{nullptr};
  HANDLE readEvent_ = nullptr;     // 工作线程独占
  char readBuffer_[8192];          // 工作线程独占
  Moqi::Proto::FrameBuffer frames_;  // 工作线程独占：字节流按长度前缀组帧
  unsigned int seqNum_ = 0;        // 工作线程独占
};

} // namespace MoqiPanel
