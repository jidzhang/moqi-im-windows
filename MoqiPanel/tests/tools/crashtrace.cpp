// crashtrace -- a minimal native debugger used only for crash forensics.
//
// Runs a child process under DEBUG_ONLY_THIS_PROCESS. On the child's first
// access violation it prints the faulting address and a symbolized stack of
// the faulting thread (module!function+offset, plus file:line when the PDB
// is available), then kills the child. C++ exceptions (0xE06D7363) are passed
// through untouched so the child's own handlers still run.
//
// Build (32-bit, so CONTEXT/StackWalk64 match the x86 daemon):
//   cmake --build MoqiPanel/build-x86 --config Release --target crashtrace
// Run:
//   crashtrace.exe [--symbols "dir1;dir2"] -- MoqiDaemon.exe --serve --portable
//
// The child inherits this process's environment and working directory.
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

HANDLE g_process = nullptr;
bool g_symbolsReady = false;

void printStack(HANDLE thread, DWORD threadId, const EXCEPTION_RECORD& record) {
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_FULL;
  if (!::GetThreadContext(thread, &context)) {
    std::printf("  (GetThreadContext failed: %lu)\n", ::GetLastError());
    return;
  }
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Eip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Ebp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Esp;
  frame.AddrStack.Mode = AddrModeFlat;
  std::printf("  thread %lu, eip=0x%08lX", threadId, static_cast<unsigned long>(context.Eip));
  if (record.NumberParameters >= 2) {
    std::printf(" (%s address 0x%08lX)",
                record.ExceptionInformation[0] == 0 ? "read" : "write",
                static_cast<unsigned long>(record.ExceptionInformation[1]));
  }
  std::printf("\n");
  for (int index = 0; index < 48; ++index) {
    if (!::StackWalk64(IMAGE_FILE_MACHINE_I386, g_process, thread, &frame, &context, nullptr,
                       ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    const DWORD64 pc = frame.AddrPC.Offset;
    IMAGEHLP_MODULE64 module = {};
    module.SizeOfStruct = sizeof(module);
    std::string moduleName = "?";
    DWORD64 moduleBase = 0;
    if (::SymGetModuleInfo64(g_process, pc, &module)) {
      moduleName = module.ModuleName;
      moduleBase = module.BaseOfImage;
    }
    char line[1024] = {};
    char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (::SymFromAddr(g_process, pc, &displacement, symbol)) {
      std::snprintf(line, sizeof(line), "    #%02d %s!%s+0x%llx", index, moduleName.c_str(),
                    symbol->Name, static_cast<unsigned long long>(displacement));
    } else {
      std::snprintf(line, sizeof(line), "    #%02d %s+0x%llx", index, moduleName.c_str(),
                    static_cast<unsigned long long>(moduleBase ? pc - moduleBase : 0));
    }
    IMAGEHLP_LINE64 source = {};
    source.SizeOfStruct = sizeof(source);
    DWORD sourceDisplacement = 0;
    if (::SymGetLineFromAddr64(g_process, pc, &sourceDisplacement, &source)) {
      std::strncat(line, "  at ", sizeof(line) - std::strlen(line) - 1);
      std::strncat(line, source.FileName, sizeof(line) - std::strlen(line) - 1);
      const size_t used = std::strlen(line);
      std::snprintf(line + used, sizeof(line) - used, ":%lu", source.LineNumber);
    }
    const size_t used = std::strlen(line);
    std::snprintf(line + used, sizeof(line) - used, "  (0x%08llX)",
                  static_cast<unsigned long long>(pc));
    std::printf("%s\n", line);
  }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  // 无缓冲：调用方靠读标准输出得知"子进程 pid"和崩溃栈，不等管道刷盘。
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int index = 1;
  std::wstring symbolPath;
  if (index < argc && std::wcscmp(argv[index], L"--symbols") == 0 && index + 1 < argc) {
    symbolPath = argv[index + 1];
    index += 2;
  }
  if (index < argc && std::wcscmp(argv[index], L"--log") == 0 && index + 1 < argc) {
    // --log FILE: report goes to a file (used when there is no usable stdout,
    // e.g. started through the tramp from a GUI parent).
    if (::_wfreopen(argv[index + 1], L"w", stdout) == nullptr) return 2;
    index += 2;
  }
  if (index < argc && std::wcscmp(argv[index], L"--") == 0) ++index;
  if (index >= argc) {
    std::fwprintf(stderr,
                  L"usage: crashtrace [--symbols \"dir1;dir2\"] [--log FILE] -- program [args...]\n");
    return 2;
  }
  std::wstring commandLine;
  for (int i = index; i < argc; ++i) {
    commandLine += L"\"";
    commandLine += argv[i];
    commandLine += L"\" ";
  }
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!::CreateProcessW(argv[index], commandLine.data(), nullptr, nullptr, FALSE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
    std::fwprintf(stderr, L"crashtrace: CreateProcess failed (%lu)\n", ::GetLastError());
    return 2;
  }
  ::CloseHandle(process.hThread);
  ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);

  for (;;) {
    DEBUG_EVENT event = {};
    if (!::WaitForDebugEvent(&event, INFINITE)) break;
    DWORD status = DBG_EXCEPTION_NOT_HANDLED;
    switch (event.dwDebugEventCode) {
      case CREATE_PROCESS_DEBUG_EVENT:
        g_process = event.u.CreateProcessInfo.hProcess;
        if (event.u.CreateProcessInfo.hThread != nullptr) {
          ::CloseHandle(event.u.CreateProcessInfo.hThread);
        }
        // 这里不初始化符号（见下面 EXCEPTION_DEBUG_EVENT 处的说明）。
        std::printf("crashtrace: child pid %lu\n", event.dwProcessId);
        status = DBG_CONTINUE;
        break;
      case EXCEPTION_DEBUG_EVENT: {
        const EXCEPTION_DEBUG_INFO& info = event.u.Exception;
        const DWORD code = info.ExceptionRecord.ExceptionCode;
        if (code == EXCEPTION_BREAKPOINT) {
          status = DBG_CONTINUE;  // initial process breakpoint
          break;
        }
        if (code == 0xE06D7363 /* C++ exception, first or second chance */ &&
            info.dwFirstChance != 0) {
          break;  // let the child's own handler run
        }
        // 调用栈要符号。SymInitialize(fInvadeProcess=TRUE) 要读子进程的模块表，
        // 而"进程创建"事件时它还没建好（实测报 0x8000000D），所以放到第一次
        // 异常时再做——那时进程早就初始化完了。
        if (!g_symbolsReady) {
          g_symbolsReady = ::SymInitializeW(g_process, symbolPath.c_str(), TRUE) != FALSE ||
                           ::SymInitializeW(g_process, nullptr, TRUE) != FALSE;
        }
        std::printf("crashtrace: exception 0x%08lX at 0x%p (%s chance), symbols %s\n",
                    static_cast<unsigned long>(code), info.ExceptionRecord.ExceptionAddress,
                    info.dwFirstChance ? "first" : "second", g_symbolsReady ? "on" : "OFF");
        if (g_symbolsReady) {
          HANDLE thread = ::OpenThread(THREAD_ALL_ACCESS, FALSE, event.dwThreadId);
          if (thread != nullptr) {
            printStack(thread, event.dwThreadId, info.ExceptionRecord);
            ::CloseHandle(thread);
          }
        }
        if (info.dwFirstChance == 0 || code == EXCEPTION_ACCESS_VIOLATION) {
          std::fflush(stdout);
          ::TerminateProcess(g_process, 1);
          ::ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
          return 1;
        }
        break;
      }
      case EXIT_PROCESS_DEBUG_EVENT:
        std::printf("crashtrace: child exited, status 0x%08lX\n",
                    static_cast<unsigned long>(event.u.ExitProcess.dwExitCode));
        ::ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
        return 0;
      case CREATE_THREAD_DEBUG_EVENT:
        if (event.u.CreateThread.hThread != nullptr) {
          ::CloseHandle(event.u.CreateThread.hThread);
        }
        status = DBG_CONTINUE;
        break;
      case LOAD_DLL_DEBUG_EVENT:
        if (event.u.LoadDll.hFile != nullptr) {
          ::CloseHandle(event.u.LoadDll.hFile);
        }
        status = DBG_CONTINUE;
        break;
      default:
        status = DBG_CONTINUE;
        break;
    }
    ::ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
  }
  std::printf("crashtrace: debug loop ended (%lu)\n", ::GetLastError());
  return 3;
}
