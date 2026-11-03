#ifndef _Moqi_KEY_GUARD_LOG_H_
#define _Moqi_KEY_GUARD_LOG_H_

#include <string>

namespace Moqi {

// 按键被放行直通/丢失这类异常事件的无条件日志（默认 error 档也写），
// 实现 见 MoqiClient.cpp：落在 %LOCALAPPDATA%\MoqiIM\Log\tsf-key-guard.log。
void appendRpcGuardLog(const std::wstring &message);

// 高频/含用户内容的诊断（按键状态、AI 文本、轮询细节）：默认不写，
// 只有 MoqiLaunch(er).json 里 logLevel=debug/trace 时才落盘。
void appendRpcTraceLog(const std::wstring &message);

} // namespace Moqi

#endif // _Moqi_KEY_GUARD_LOG_H_
