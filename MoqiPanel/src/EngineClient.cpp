#include <windows.h>
#include <objbase.h>

#include <lmcons.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>

#include "EngineClient.h"
#include "PanelCore.h"
#include "PanelMessages.h"

#pragma comment(lib, "ole32.lib")

namespace MoqiPanel {
namespace {

// 首键可能撞上首次部署（daemon 侧 ensureEngine 最长 30 秒），所以按键类 RPC
// 的等待上限必须大于它；平时应答在毫秒级。
constexpr DWORD kKeyTimeoutMs = 35000;
// ON_MENU 当"引擎就绪屏障"用：它内部会等首次部署完成。
constexpr DWORD kReadyBarrierTimeoutMs = 40000;
constexpr DWORD kControlTimeoutMs = 5000;
constexpr int kMaxConnectAttempts = 4;

bool isReturnValueSet(const Json::Value& frame) {
  const Json::Value& v = frame["returnValue"];
  if (v.isBool()) return v.asBool();
  if (v.isIntegral()) return v.asInt() != 0;
  return false;
}

std::wstring toHex8(uint32_t value) {
  wchar_t buf[16] = {};
  swprintf_s(buf, L"%08x", value);
  return buf;
}

// 同一台机器上可能有多个输入板副本：管道/互斥体名带 exe 目录的哈希，互不
// 串台，也绝不与已安装的墨奇输入法（默认名）撞名——两套引擎可以并存。
uint32_t fnv1aOfDir(const std::wstring& dir) {
  uint32_t h = 2166136261u;
  for (wchar_t c : dir) {
    c = static_cast<wchar_t>(towlower(c));
    h ^= static_cast<uint32_t>(c) & 0xFFu;
    h *= 16777619u;
    h ^= static_cast<uint32_t>(c) >> 8;
    h *= 16777619u;
  }
  return h;
}

Json::Value keyParams(const KeyInput& key) {
  Json::Value event(Json::objectValue);
  event["charCode"] = key.charCode;
  event["keyCode"] = key.vk;
  event["repeatCount"] = 1;
  event["scanCode"] = key.scanCode;
  event["isExtended"] = key.isExtended;
  // 与 TSF DLL 同策略：修饰键全零时省掉整个数组（省一次 256 节点解析）。
  bool anyState = false;
  for (BYTE state : key.states) {
    if (state != 0) {
      anyState = true;
      break;
    }
  }
  if (anyState) {
    Json::Value states(Json::arrayValue);
    for (BYTE state : key.states) states.append(state);
    event["keyStates"] = states;
  }
  Json::Value params(Json::objectValue);
  params["keyEvent"] = event;
  return params;
}

// 面板自己合成的按键：不读真实键盘，修饰键状态显式给。
KeyInput syntheticKey(UINT vk, UINT scanCode, bool isUp) {
  KeyInput key;
  key.vk = vk;
  key.scanCode = scanCode;
  key.isUp = isUp;
  return key;
}

// 左 Shift 按下/抬起。GetKeyboardState 里 VK_SHIFT 与 VK_LSHIFT 是两个键位，
// 引擎的 translateModifiers 读 VK_SHIFT、shiftSide 靠扫描码 0x2A 认左 Shift。
KeyInput leftShiftKey(bool isUp) {
  KeyInput key = syntheticKey(VK_SHIFT, 0x2A, isUp);
  if (!isUp) {
    key.states[VK_SHIFT] = 0x80;
    key.states[VK_LSHIFT] = 0x80;
  }
  return key;
}

} // namespace

// ---- 公开接口（任意线程） -------------------------------------------------

void EngineClient::start(HWND notify, const std::wstring& exeDir) {
  notify_ = notify;
  exeDir_ = exeDir;
  wchar_t user[UNLEN + 1] = {};
  DWORD userLen = UNLEN + 1;
  std::wstring userPart;
  if (::GetUserNameW(user, &userLen)) userPart = user;
  DWORD sessionId = 0;
  ::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId);
  const std::wstring suffix =
      std::to_wstring(sessionId) + L"." + toHex8(fnv1aOfDir(exeDir));
  pipeName_ = L"\\\\.\\pipe\\" + userPart + L"\\MoqiIM\\Panel." + suffix;
  mutexName_ = L"MoqiPanelMutex." + suffix;
  wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  done_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread([this] { threadMain(); });
}

bool EngineClient::stop(DWORD waitMs) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_ = true;
  }
  if (wake_ != nullptr) ::SetEvent(wake_);
  if (!thread_.joinable()) return true;
  if (::WaitForSingleObject(done_, waitMs) != WAIT_OBJECT_0) {
    // 还卡在某个 RPC 里（首次部署可达数十秒）：取消它的 I/O，再给一小会儿。
    const HANDLE pipe = pipe_.load();
    if (pipe != nullptr) ::CancelIoEx(pipe, nullptr);
    if (::WaitForSingleObject(done_, 2000) != WAIT_OBJECT_0) {
      // 进程马上就退出了：放任线程被系统回收。本对象此后不可再碰。
      thread_.detach();
      return false;
    }
  }
  thread_.join();
  return true;
}

void EngineClient::pushTask(Task task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(task));
  }
  ::SetEvent(wake_);
}

bool EngineClient::popTask(Task* task, DWORD waitMs) {
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!queue_.empty()) {
        *task = std::move(queue_.front());
        queue_.pop_front();
        return true;
      }
      if (stopRequested_) return false;
    }
    if (::WaitForSingleObject(wake_, waitMs) != WAIT_OBJECT_0) return false;
  }
}

void EngineClient::postKey(const KeyInput& key, uint64_t keyId) {
  Task task;
  task.kind = TaskKind::Key;
  task.key = key;
  task.keyId = keyId;
  pushTask(std::move(task));
}

void EngineClient::postSelectCandidate(int index) {
  Task task;
  task.kind = TaskKind::SelectCandidate;
  task.index = index;
  pushTask(std::move(task));
}

void EngineClient::postChangePage(bool backward) {
  Task task;
  task.kind = TaskKind::ChangePage;
  task.backward = backward;
  pushTask(std::move(task));
}

void EngineClient::postShiftTap() {
  pushTask(Task{TaskKind::ShiftTap});
}

void EngineClient::postFlush() {
  pushTask(Task{TaskKind::Flush});
}

// ---- 工作线程 ---------------------------------------------------------------

void EngineClient::setStatus(EngineStatus status) {
  ::PostMessageW(notify_, WM_APP_ENGINE_STATUS, static_cast<WPARAM>(status), 0);
}

void EngineClient::postResult(std::unique_ptr<EngineResult> result) {
  EngineResult* raw = result.get();
  if (::PostMessageW(notify_, WM_APP_ENGINE_RESULT, 0,
                     reinterpret_cast<LPARAM>(raw))) {
    (void)result.release();
  }
}

bool EngineClient::daemonAlive() const {
  const HANDLE process = daemonProcess_.load();
  return process != nullptr && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

std::wstring EngineClient::buildChildEnvironment() const {
  // MOQI_* 是开发/评测用的接缝变量，不能漏进绿色版自带的 daemon（便携模式
  // 需要的那两个下面显式给）。
  std::wstring env;
  LPWCH block = ::GetEnvironmentStringsW();
  for (LPWCH p = block; p != nullptr && *p != L'\0';) {
    const std::wstring entry(p);
    p += entry.size() + 1;
    if (_wcsnicmp(entry.c_str(), L"MOQI_", 5) == 0) continue;
    env += entry;
    env.push_back(L'\0');
  }
  if (block != nullptr) ::FreeEnvironmentStringsW(block);
  env += L"MOQI_SERVE_PIPE=" + pipeName_;
  env.push_back(L'\0');
  env += L"MOQI_SERVE_MUTEX=" + mutexName_;
  env.push_back(L'\0');
  env.push_back(L'\0');
  return env;
}

bool EngineClient::spawnDaemon() {
  const std::wstring exe = exeDir_ + L"\\MoqiDaemon.exe";
  std::wstring cmd = L"\"" + exe + L"\" --serve --portable";
  std::wstring env = buildChildEnvironment();
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(),
                        exeDir_.c_str(), &si, &pi)) {
    return false;
  }
  ::CloseHandle(pi.hThread);
  daemonProcess_.store(pi.hProcess);
  return true;
}

// 面板包自带的引擎数据补丁（data\default.custom.yaml：标点原样输出）不在 librime
// 的部署脏检查里——共享目录里的 *.custom.yaml 只在编译那一刻被读到。补丁变了
//（升级包带来新补丁、或被删掉改回中文标点）而 user\build 还是旧编译产物时，
// 引擎会一直用旧产物（实测：旧文件夹原地覆盖升级后仍是中文标点）。这里按补丁
// 内容留一个戳：戳不对就把部署状态退回"未部署"，让引擎重新部署：
//   - 删 user\build（可再生的编译产物；只删它不够——引擎会直接拿包里的预编译
//     产物，那份是不带补丁的）；
//   - 删 user\user.yaml（librime 在里面记"上次部署时间"，rimeStartMaintenance
//     以它判断有没有改动；里面只有开关记忆，下次启动自动重建）。
// 实测（2026-11-27）：只删 build → 仍是不带补丁的旧配置；再删 user.yaml →
// 重新编译，补丁生效。用户词典在 user\ 根下，不受影响。
// 本文件夹还有 daemon 活着（哪怕正在退出）就不动它的文件：此时要么连得上它
//（不会走到这里），要么它正占着这些文件。单实例互斥体随 daemon 进程存亡，
// 拿它当"有没有 daemon"的探针。
void EngineClient::refreshDataPatch() {
  namespace fs = std::filesystem;
  const HANDLE living = ::OpenMutexW(SYNCHRONIZE, FALSE, mutexName_.c_str());
  if (living != nullptr) {
    ::CloseHandle(living);
    return;
  }
  if (::GetLastError() != ERROR_FILE_NOT_FOUND) return;  // 存在但打不开：当它在
  const fs::path root(exeDir_);
  const fs::path patch =
      root / L"moqi-ime\\input_methods\\rime\\data\\default.custom.yaml";
  std::string content;
  {
    std::ifstream in(patch, std::ios::binary);
    if (in) {
      content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  const std::string current = contentStamp(content);
  std::string saved;
  {
    std::ifstream in(root / L"user\\panel-data.stamp", std::ios::binary);
    if (in) in >> saved;
  }
  if (saved == current) return;
  std::error_code ec;
  fs::remove_all(root / L"user\\build", ec);
  if (ec) return;  // 删不掉就不写戳，下次启动再试
  fs::remove(root / L"user\\user.yaml", ec);
  if (ec) return;
  fs::create_directories(root / L"user", ec);
  std::ofstream out(root / L"user\\panel-data.stamp", std::ios::binary | std::ios::trunc);
  out << current;
}

void EngineClient::closePipe() {
  const HANDLE pipe = pipe_.exchange(nullptr);
  if (pipe != nullptr) {
    ::CancelIoEx(pipe, nullptr);
    ::CloseHandle(pipe);
  }
  frames_.clear();
}

// 连一次管道。成功时管道句柄已存入 pipe_。
bool EngineClient::tryOpenPipe() {
  const HANDLE pipe = ::CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                                    nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  pipe_.store(pipe);
  return true;
}

bool EngineClient::connectOnce() {
  // 上一个 daemon 还在退出：等它走干净（便携 daemon 随末连接断开自退），
  // 否则新实例会因同名管道还在而直接退出。
  const HANDLE previous = daemonProcess_.exchange(nullptr);
  if (previous != nullptr) {
    if (::WaitForSingleObject(previous, 4000) != WAIT_OBJECT_0) {
      ::TerminateProcess(previous, 0);
    }
    ::CloseHandle(previous);
  }
  // 已有实例在服务（上次面板刚退出时它可能还没来得及走）就直接用。
  bool opened = tryOpenPipe();
  if (!opened) {
    // 要自己拉 daemon：先确认部署产物与自带的数据补丁一致。
    refreshDataPatch();
    if (!spawnDaemon()) return false;
    const ULONGLONG deadline = ::GetTickCount64() + 30000;
    while (!(opened = tryOpenPipe())) {
      if (!daemonAlive()) return false;
      if (::GetTickCount64() > deadline) return false;
      // 短轮询：管道一出现就连上。daemon 刚绑好管道时引擎还在初始化，连得
      // 越早，INIT/ON_ACTIVATE 越是落在 rime.dll 加载之前（daemon 侧另有
      // DaemonMenuService::gather 的"引擎未就绪不调 librime"守门）。
      ::Sleep(5);
    }
  }
  if (readEvent_ != nullptr) ::CloseHandle(readEvent_);
  readEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  frames_.clear();

  // INIT / ON_ACTIVATE 在 daemon 侧不等引擎初始化，回得很快。
  Json::Value reply;
  std::vector<Json::Value> async;
  Json::Value init(Json::objectValue);
  GUID guid = {};
  wchar_t guidText[64] = {};
  if (SUCCEEDED(::CoCreateGuid(&guid)) &&
      ::StringFromGUID2(guid, guidText, 64) > 0) {
    init["guid"] = wideToUtf8(guidText);
  }
  init["isWindows8Above"] = true;
  init["isKeyboardOpen"] = true;
  init["isMetroApp"] = false;
  init["isUiLess"] = false;
  init["isConsole"] = false;
  if (!rpc("METHOD_INIT", init, kControlTimeoutMs, &reply, &async)) return false;
  Json::Value activate(Json::objectValue);
  activate["isKeyboardOpen"] = true;
  if (!rpc("METHOD_ON_ACTIVATE", activate, kControlTimeoutMs, &reply, &async)) {
    return false;
  }
  // 就绪屏障：ON_MENU 在 daemon 侧会等首次部署完成再回。超时不算连接失败
  // ——首键还会再等一次；这样界面能在引擎真正可用之后才显示"就绪"。
  rpc("METHOD_ON_MENU", Json::Value(Json::objectValue), kReadyBarrierTimeoutMs,
      &reply, &async);
  if (pipe_.load() == nullptr) return false;
  return true;
}

bool EngineClient::writeAll(const std::string& data) {
  const HANDLE pipe = pipe_.load();
  if (pipe == nullptr || data.empty()) return false;
  HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) return false;
  OVERLAPPED ov = {};
  ov.hEvent = event;
  DWORD written = 0;
  BOOL ok = ::WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, &ov);
  if (!ok) {
    if (::GetLastError() != ERROR_IO_PENDING ||
        ::WaitForSingleObject(event, 5000) != WAIT_OBJECT_0 ||
        !::GetOverlappedResult(pipe, &ov, &written, FALSE)) {
      ::CancelIoEx(pipe, &ov);
      ::CloseHandle(event);
      return false;
    }
  }
  ::CloseHandle(event);
  return written == data.size();
}

bool EngineClient::readFrame(DWORD timeoutMs, std::string* payload) {
  const HANDLE pipe = pipe_.load();
  if (pipe == nullptr) return false;
  for (;;) {
    if (frames_.nextFrame(*payload)) return true;
    if (frames_.overflowed()) return false;  // 长度前缀损坏：帧序已不可信
    ::ResetEvent(readEvent_);
    OVERLAPPED ov = {};
    ov.hEvent = readEvent_;
    DWORD got = 0;
    if (!::ReadFile(pipe, readBuffer_, sizeof(readBuffer_), &got, &ov)) {
      if (::GetLastError() != ERROR_IO_PENDING) return false;  // 对端断开
      if (::WaitForSingleObject(readEvent_, timeoutMs) != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe, &ov);
        // 取消也要等它落地：句柄上不留悬空 I/O。万一取消前已经读到字节，
        // 那些字节是流里的真实数据，必须收下，否则帧序错位。
        if (::GetOverlappedResult(pipe, &ov, &got, TRUE) && got > 0) {
          frames_.append(readBuffer_, got);
        }
        return false;
      }
      if (!::GetOverlappedResult(pipe, &ov, &got, FALSE)) return false;
    }
    if (got == 0) return false;
    frames_.append(readBuffer_, got);
  }
}

bool EngineClient::rpc(const std::string& method, const Json::Value& params,
                       DWORD timeoutMs, Json::Value* reply,
                       std::vector<Json::Value>* asyncOut) {
  const HANDLE pipe = pipe_.load();
  if (pipe == nullptr) return false;
  Json::Value request(Json::objectValue);
  request["method"] = method;
  request["seqNum"] = ++seqNum_;
  if (params.isObject()) {
    for (auto it = params.begin(); it != params.end(); ++it) {
      request[it.name()] = *it;
    }
  }
  std::string framed;
  if (!Moqi::Proto::serializeJsonMessage(request, framed) || !writeAll(framed)) {
    return false;
  }
  // 应答前先到的帧是异步推送（seqNum 对不上）：收起来交给界面，继续等本帧。
  for (int guard = 0; guard < 64; ++guard) {
    std::string payload;
    if (!readFrame(timeoutMs, &payload)) return false;
    Json::Value message;
    if (!Moqi::Proto::parseJsonPayload(payload, message) || !message.isObject()) {
      return false;
    }
    const Json::Value& seq = message["seqNum"];
    if (seq.isIntegral() && seq.asUInt() == seqNum_) {
      *reply = std::move(message);
      return true;
    }
    if (asyncOut != nullptr) asyncOut->push_back(std::move(message));
  }
  return false;
}

void EngineClient::threadMain() {
  int failures = 0;
  bool connected = false;
  for (;;) {
    bool stopping = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping = stopRequested_ && queue_.empty();
    }
    if (stopping) break;
    if (!connected) {
      if (failures >= kMaxConnectAttempts) {
        setStatus(EngineStatus::Failed);
        // 失败态：积压与后续任务立刻判失败——界面把按键原样放行，绝不吞键。
        Task task;
        while (popTask(&task, 200)) {
          auto result = std::make_unique<EngineResult>();
          result->keyId = task.keyId;
          result->isFlush = task.kind == TaskKind::Flush;
          postResult(std::move(result));
        }
        continue;
      }
      setStatus(EngineStatus::Connecting);
      connected = connectOnce();
      if (!connected) {
        ++failures;
        closePipe();
        ::Sleep(800);
        continue;
      }
      failures = 0;
      setStatus(EngineStatus::Ready);
    }
    Task task;
    if (!popTask(&task, 200)) continue;
    if (!runTask(task)) {
      // 管道断了：本任务已按失败回包。收摊重连。
      closePipe();
      connected = false;
      setStatus(EngineStatus::Connecting);
    }
  }
  // 收尾：尽量通知引擎反激活（学习数据落盘的安全窗口），然后断管。
  if (connected) {
    Json::Value reply;
    std::vector<Json::Value> async;
    rpc("METHOD_ON_DEACTIVATE", Json::Value(Json::objectValue), 1500, &reply, &async);
  }
  closePipe();
  ::SetEvent(done_);
}

bool EngineClient::runTask(const Task& task) {
  auto result = std::make_unique<EngineResult>();
  result->keyId = task.keyId;
  result->isFlush = task.kind == TaskKind::Flush;
  std::vector<Json::Value> async;

  // 一键 = FILTER（判吃没吃）+ 吃了才发 ON。与 TSF 的两阶段完全一致。
  auto processKey = [&](const KeyInput& key, bool recordEaten) -> bool {
    const Json::Value params = keyParams(key);
    Json::Value filterReply;
    if (!rpc(key.isUp ? "METHOD_FILTER_KEY_UP" : "METHOD_FILTER_KEY_DOWN", params,
             kKeyTimeoutMs, &filterReply, &async)) {
      return false;
    }
    const bool eaten = isReturnValueSet(filterReply);
    result->frames.push_back(std::move(filterReply));
    if (eaten) {
      Json::Value onReply;
      if (!rpc(key.isUp ? "METHOD_ON_KEY_UP" : "METHOD_ON_KEY_DOWN", params,
               kKeyTimeoutMs, &onReply, &async)) {
        return false;
      }
      result->frames.push_back(std::move(onReply));
    }
    if (recordEaten) result->eaten = eaten;
    return true;
  };

  bool ok = false;
  switch (task.kind) {
    case TaskKind::Key:
      ok = processKey(task.key, true);
      break;
    case TaskKind::ShiftTap:
      ok = processKey(leftShiftKey(false), false) &&
           processKey(leftShiftKey(true), false);
      break;
    case TaskKind::Flush:
      ok = processKey(syntheticKey(VK_SPACE, 0x39, false), false) &&
           processKey(syntheticKey(VK_SPACE, 0x39, true), false);
      if (ok) result->flushed = true;
      break;
    case TaskKind::SelectCandidate: {
      Json::Value params(Json::objectValue);
      params["candidateIndex"] = task.index;
      Json::Value reply;
      ok = rpc("METHOD_SELECT_CANDIDATE", params, kKeyTimeoutMs, &reply, &async);
      if (ok) result->frames.push_back(std::move(reply));
      break;
    }
    case TaskKind::ChangePage: {
      Json::Value params(Json::objectValue);
      params["pageBackward"] = task.backward;
      Json::Value reply;
      ok = rpc("METHOD_CHANGE_PAGE", params, kKeyTimeoutMs, &reply, &async);
      if (ok) result->frames.push_back(std::move(reply));
      break;
    }
  }
  result->ok = ok;

  // 顺带把管道里已经躺着的异步帧取走（非阻塞：没数据立刻返回）。
  const HANDLE pipe = pipe_.load();
  if (ok && pipe != nullptr) {
    DWORD available = 0;
    while (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) &&
           available > 0) {
      std::string payload;
      if (!readFrame(100, &payload)) break;
      Json::Value message;
      if (Moqi::Proto::parseJsonPayload(payload, message) && message.isObject()) {
        async.push_back(std::move(message));
      }
    }
  }
  // 异步帧在时间上先于本任务的结果，先送。
  if (!async.empty()) {
    auto pushed = std::make_unique<EngineResult>();
    pushed->ok = true;
    pushed->frames = std::move(async);
    postResult(std::move(pushed));
  }
  postResult(std::move(result));
  return ok;
}

} // namespace MoqiPanel
