// crashhook.h -- test-only. Force-included (/FI) into a forensic build of
// MoqiDaemon so that an access violation in the REAL start-up path leaves a
// symbolized stack behind without a debugger attached (a debugger changes
// thread timing enough to hide the race being investigated).
//
// Build (separate tree; product sources untouched):
//   cmake -S moqi-daemon -B moqi-daemon/build-x86-hook -G "Visual Studio 17 2022" -A Win32 ^
//         -DCMAKE_CXX_FLAGS_RELEASE="-O2 -Ob2 -DNDEBUG -Zi -Gy -FI<abs path>/crashhook.h" ^
//         -DCMAKE_EXE_LINKER_FLAGS_RELEASE="-DEBUG -INCREMENTAL:NO"
// Environment (read at crash time): CRASHHOOK_LOG, CRASHHOOK_SYMBOLS.
// After writing the report the exception is handed back to the system, so the
// process still dies exactly as it would without the hook.
//
// /FI also reaches every other project of the build tree (jsoncpp is C++11 and
// must not see windows.h), hence the language-level guard below: the daemon
// itself is C++20 and compiles with the full windows.h anyway.
#pragma once

#if defined(_MSC_VER) && defined(_WIN32) && __cplusplus >= 201703L

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace moqi_crashhook {

inline void writeReport(EXCEPTION_POINTERS* info) {
  const wchar_t* logPath = ::GetEnvironmentVariableW(L"CRASHHOOK_LOG", nullptr, 0) > 0
                               ? _wgetenv(L"CRASHHOOK_LOG")
                               : nullptr;
  if (!logPath || !*logPath) return;
  FILE* log = _wfopen(logPath, L"a");
  if (!log) return;
  HANDLE process = ::GetCurrentProcess();
  ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
  const wchar_t* symbolPath =
      ::GetEnvironmentVariableW(L"CRASHHOOK_SYMBOLS", nullptr, 0) > 0
          ? _wgetenv(L"CRASHHOOK_SYMBOLS")
          : nullptr;
  ::SymInitializeW(process, symbolPath, TRUE);
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  std::fprintf(log, "crashhook: exception 0x%08lX at %p, thread %lu\n",
               static_cast<unsigned long>(record.ExceptionCode), record.ExceptionAddress,
               ::GetCurrentThreadId());
  if (record.NumberParameters >= 2) {
    std::fprintf(log, "  %s address 0x%08lX\n",
                 record.ExceptionInformation[0] == 0 ? "read" : "write",
                 static_cast<unsigned long>(record.ExceptionInformation[1]));
  }
  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Eip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Ebp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Esp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int index = 0; index < 64; ++index) {
    if (!::StackWalk64(IMAGE_FILE_MACHINE_I386, process, ::GetCurrentThread(), &frame, &context,
                       nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    const DWORD64 pc = frame.AddrPC.Offset;
    IMAGEHLP_MODULE64 module = {};
    module.SizeOfStruct = sizeof(module);
    char moduleName[64] = "?";
    if (::SymGetModuleInfo64(process, pc, &module)) {
      strncpy(moduleName, module.ModuleName, sizeof(moduleName) - 1);
    }
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    char line[1024] = {};
    if (::SymFromAddr(process, pc, &displacement, symbol)) {
      std::snprintf(line, sizeof(line), "    #%02d %s!%s+0x%llx", index, moduleName, symbol->Name,
                    static_cast<unsigned long long>(displacement));
    } else {
      std::snprintf(line, sizeof(line), "    #%02d %s+0x%llx", index, moduleName,
                    static_cast<unsigned long long>(pc));
    }
    IMAGEHLP_LINE64 source = {};
    source.SizeOfStruct = sizeof(source);
    DWORD sourceDisplacement = 0;
    if (::SymGetLineFromAddr64(process, pc, &sourceDisplacement, &source)) {
      size_t used = strlen(line);
      snprintf(line + used, sizeof(line) - used, "  at %s:%lu", source.FileName,
               source.LineNumber);
    }
    std::fprintf(log, "%s  (0x%08llX)\n", line, static_cast<unsigned long long>(pc));
  }
  std::fclose(log);
}

inline LONG WINAPI filter(EXCEPTION_POINTERS* info) {
  __try {
    writeReport(info);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // never let the diagnostic itself change how the process dies
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

struct Installer {
  Installer() { ::SetUnhandledExceptionFilter(&filter); }
};

inline const Installer installer;  // one instance for the whole image

}  // namespace moqi_crashhook

#endif  // _MSC_VER && _WIN32 && C++17
