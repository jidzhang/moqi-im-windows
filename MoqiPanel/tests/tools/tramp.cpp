// tramp -- stands in for MoqiDaemon.exe so that the REAL panel spawn path can
// be observed under a debugger. The panel launches <folder>\MoqiDaemon.exe;
// with this tramp in its place the chain becomes
//
//   MoqiPanel.exe -> MoqiDaemon.exe (tramp) -> crashtrace.exe -> MoqiDaemon.real.exe
//
// so parent/child relations, environment, pipe/mutex names and timing are the
// panel's own; only the daemon under test runs debugged. The tramp waits for
// the chain and returns the daemon's exit code.
//
// Environment consumed (set by tests/daemon_crashtrace.py --client tramp):
//   CRASHTRACE_SYMBOLS  dbghelp symbol path
//   CRASHTRACE_LOG      file crashtrace writes its report to
#include <windows.h>

#include <string>

int wmain() {
  wchar_t own[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, own, MAX_PATH);
  std::wstring dir(own, length);
  dir.resize(dir.find_last_of(L'\\'));
  auto env = [](const wchar_t* name) {
    const wchar_t* value = ::GetEnvironmentVariableW(name, nullptr, 0) == 0
                               ? L""
                               : _wgetenv(name);
    return std::wstring(value ? value : L"");
  };
  const std::wstring symbols = env(L"CRASHTRACE_SYMBOLS");
  const std::wstring logFile = env(L"CRASHTRACE_LOG");
  if (logFile.empty()) return 0xC0000135;  // not launched by the harness
  std::wstring command = L"\"" + dir + L"\\crashtrace.exe\" --symbols \"" + symbols +
                         L"\" --log \"" + logFile + L"\" -- \"" + dir +
                         L"\\MoqiDaemon.real.exe\" --serve --portable";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!::CreateProcessW((dir + L"\\crashtrace.exe").c_str(), command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup, &process)) {
    return static_cast<int>(::GetLastError());
  }
  ::CloseHandle(process.hThread);
  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(process.hProcess, &code);
  ::CloseHandle(process.hProcess);
  return static_cast<int>(code);
}
