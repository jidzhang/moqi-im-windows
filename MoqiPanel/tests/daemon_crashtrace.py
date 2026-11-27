# -*- coding: utf-8 -*-
"""Capture the stack of the portable daemon's start-up access violation.

ProcMon showed that the first MoqiDaemon.exe --serve --portable of one panel
launch can die with 0xC0000005 about 0.26 s after start, while the panel is
already connected and firing INIT / ON_ACTIVATE / ON_MENU; the panel then
silently reconnects to a second daemon. The crash is timing dependent (it was
never seen at normal I/O speed, only with ProcMon slowing every file operation
down ~20x), so this harness removes the guesswork:

  1. copy a symbol-rich daemon build into the panel folder as MoqiDaemon.sym.exe
     (same code as the shipped daemon plus debug info; it finds rime.dll and
     data next to itself exactly like the real one)
  2. launch it under tests/tools/crashtrace.exe -- a tiny debugger that prints
     a symbolized stack on the first access violation
  3. compute the pipe/mutex names the panel will use for this folder and hand
     them to the daemon, so the panel finds "an instance already serving"
     and connects instead of spawning its own daemon
  4. launch the real MoqiPanel.exe, let it connect, then close it
  5. --procmon keeps ProcMon capturing during the loop (the slow I/O that
     triggers the race)

Usage:
  python MoqiPanel/tests/daemon_crashtrace.py --rounds 12 [--procmon]
      [--sym-daemon moqi-daemon/build-x86-sym/Release/MoqiDaemon.exe]

Build the symbol daemon (does not touch the shipping build tree):
  cmake -S moqi-daemon -B moqi-daemon/build-x86-sym -G "Visual Studio 17 2022" -A Win32 ^
        -DCMAKE_CXX_FLAGS_RELEASE="/O2 /Ob2 /DNDEBUG /Zi /Gy" ^
        -DCMAKE_EXE_LINKER_FLAGS_RELEASE="/DEBUG /INCREMENTAL:NO"
  cmake --build moqi-daemon/build-x86-sym --config Release --target MoqiDaemon
"""
import argparse
import ctypes
import getpass
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from drive_panel import WM_CLOSE, top_windows, child_controls, window_text, ID_STATUS
from portable_daemon_check import PipeClient

INIT_REQUEST = {"guid": "{00000000-0000-0000-0000-000000000001}", "isWindows8Above": True,
                "isKeyboardOpen": True, "isMetroApp": False, "isUiLess": False,
                "isConsole": False}

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi32 = ctypes.windll.advapi32
ERROR_PIPE_BUSY = 234


def fnv1a_dir(path):
    """Mirror of EngineClient::fnv1aOfDir (C++)."""
    h = 2166136261
    for ch in path.lower():
        h ^= ord(ch) & 0xFF
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= ord(ch) >> 8
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def panel_pipe_names(folder):
    """Exactly what EngineClient::start() builds for a panel launched from <folder>."""
    user = ctypes.create_unicode_buffer(256)
    size = wintypes.DWORD(256)
    advapi32.GetUserNameW(user, ctypes.byref(size))
    session = wintypes.DWORD(0)
    kernel32.ProcessIdToSessionId(kernel32.GetCurrentProcessId(), ctypes.byref(session))
    suffix = "%d.%08x" % (session.value, fnv1a_dir(folder))
    return ("\\\\.\\pipe\\%s\\MoqiIM\\Panel.%s" % (user.value, suffix),
            "MoqiPanelMutex." + suffix)


def pipe_exists(pipe_name):
    """True once the daemon has created its pipe instance -- WITHOUT connecting
    (a client that connects and drops would make the portable daemon exit)."""
    kernel32.WaitNamedPipeW.argtypes = [ctypes.c_wchar_p, wintypes.DWORD]
    if kernel32.WaitNamedPipeW(pipe_name, 0):
        return True
    return ctypes.get_last_error() == ERROR_PIPE_BUSY


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", ".."))
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", default=os.path.join(here, "..", "dist", "MoqiPanel"))
    parser.add_argument("--sym-daemon", default=os.path.join(repo, "moqi-daemon", "build-x86-sym",
                                                             "Release", "MoqiDaemon.exe"))
    parser.add_argument("--rounds", type=int, default=12)
    parser.add_argument("--procmon", action="store_true")
    parser.add_argument("--out", default=os.path.join(here, "out", "crashtrace"))
    parser.add_argument("--client", choices=("panel", "pipe", "tramp", "hook"), default="panel",
                        help="panel: the real MoqiPanel.exe; pipe: a raw pipe client that "
                             "connects the instant the pipe appears; tramp: the panel spawns "
                             "the daemon itself through tests/tools/tramp (real spawn path, "
                             "debugger attached); hook: real spawn path, no debugger -- the "
                             "daemon build reports its own crash stack (crashhook.h)")
    parser.add_argument("--hook-daemon", default=os.path.join(repo, "moqi-daemon", "build-x86-hook",
                                                              "Release", "MoqiDaemon.exe"))
    args = parser.parse_args()
    folder = os.path.abspath(args.folder)
    # 绝对路径：tramp 模式下日志路径要交给工作目录不同的另一个进程去打开。
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    trace_tool = os.path.join(here, "..", "build-x86", "Release", "crashtrace.exe")
    sym_daemon = os.path.abspath(args.sym_daemon)
    for required in (trace_tool, sym_daemon, os.path.join(folder, "MoqiPanel.exe")):
        if not os.path.isfile(required):
            print("FATAL: missing %s" % required)
            return 2

    tramp_tool = os.path.join(here, "..", "build-x86", "Release", "tramp.exe")
    if args.client == "tramp" and not os.path.isfile(tramp_tool):
        print("FATAL: missing %s" % tramp_tool)
        return 2
    hook_daemon = os.path.abspath(args.hook_daemon)
    if args.client == "hook" and not os.path.isfile(hook_daemon):
        print("FATAL: missing %s (see tests/tools/crashhook.h for the build line)" % hook_daemon)
        return 2

    # The symbol daemon must live next to rime.dll and the data tree.
    symbol_dirs = [os.path.dirname(sym_daemon)]
    librime = os.path.join(repo, "..", "librime")
    for name in os.listdir(librime) if os.path.isdir(librime) else []:
        candidate = os.path.join(librime, name, "bin", "Release")
        if os.path.isfile(os.path.join(candidate, "rime.pdb")):
            symbol_dirs.append(candidate)
    symbols = ";".join(symbol_dirs)
    print("symbol path: %s" % symbols)

    ship = os.path.join(folder, "MoqiDaemon.exe")
    backup = os.path.join(folder, "MoqiDaemon.ship.exe")
    deployed = os.path.join(folder, "MoqiDaemon.sym.exe")
    if args.client == "tramp":
        # The panel spawns <folder>\MoqiDaemon.exe itself: swap in the tramp
        # so its own spawn path runs the symbol daemon under crashtrace.
        os.replace(ship, backup)
        shutil.copyfile(sym_daemon, os.path.join(folder, "MoqiDaemon.real.exe"))
        shutil.copyfile(tramp_tool, ship)
        shutil.copyfile(trace_tool, os.path.join(folder, "crashtrace.exe"))
    elif args.client == "hook":
        # No debugger at all: the hook build writes its own crash stack.
        os.replace(ship, backup)
        shutil.copyfile(hook_daemon, ship)
    else:
        shutil.copyfile(sym_daemon, deployed)

    procmon = None
    if args.procmon:
        procmon = r"C:\Program Files\coreutils\bin\Procmon64.exe"
        backing = os.path.join(os.environ["LOCALAPPDATA"], "Temp", "moqi-panel-procmon", "race.pml")
        if os.path.exists(backing):
            os.remove(backing)
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Start-Process -FilePath '%s' -Verb RunAs -ArgumentList "
                        "'/AcceptEula /Quiet /Minimized /BackingFile %s'" % (procmon, backing)],
                       capture_output=True, timeout=60)
        time.sleep(4)
        print("procmon capturing to %s" % backing)

    pipe_name, mutex_name = panel_pipe_names(folder)
    crashes = 0
    try:
        for index in range(args.rounds):
            env = dict(os.environ)
            log_path = os.path.join(args.out, "trace-%02d.txt" % index)
            log = None
            tracer = None
            if args.client in ("panel", "pipe"):
                env["MOQI_SERVE_PIPE"] = pipe_name
                env["MOQI_SERVE_MUTEX"] = mutex_name
                log = open(log_path, "w", encoding="utf-8")
                tracer = subprocess.Popen(
                    [trace_tool, "--symbols", symbols, "--", deployed, "--serve", "--portable"],
                    cwd=folder, env=env, stdout=log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                deadline = time.time() + 20
                while time.time() < deadline and not pipe_exists(pipe_name):
                    if tracer.poll() is not None:
                        break
                    time.sleep(0.001)
            else:
                env["CRASHTRACE_SYMBOLS"] = symbols
                env["CRASHTRACE_LOG"] = log_path
                env["CRASHHOOK_SYMBOLS"] = symbols
                env["CRASHHOOK_LOG"] = log_path
                if os.path.exists(log_path):
                    os.remove(log_path)

            if args.client == "pipe":
                # Connect within ~1 ms of the pipe appearing, then fire the
                # same three requests the panel sends.
                client = None
                try:
                    client = PipeClient(pipe_name)
                    client.call("METHOD_INIT", INIT_REQUEST)
                    client.call("METHOD_ON_ACTIVATE", {"isKeyboardOpen": True})
                    client.call("METHOD_ON_MENU")
                    time.sleep(1.5)
                except (RuntimeError, OSError) as error:
                    print("  pipe client: %s" % error)
                finally:
                    if client is not None:
                        client.f.close()

            # The real panel. It connects to the pre-started daemon (panel /
            # pipe modes) or spawns tramp -> crashtrace -> daemon (tramp mode).
            panel = subprocess.Popen([os.path.join(folder, "MoqiPanel.exe")], cwd=folder, env=env)
            ready = False
            windows = []
            deadline = time.time() + 60
            while time.time() < deadline and args.client != "pipe":
                if tracer is not None and tracer.poll() is not None:
                    break  # the daemon under test died
                windows = top_windows(panel.pid)
                if windows:
                    status = [h for h, _c, cid in child_controls(windows[0]) if cid == ID_STATUS]
                    if status and "就绪" in window_text(status[0]):
                        ready = True
                        break
                time.sleep(0.1)
            if ready:
                user32 = ctypes.windll.user32
                user32.PostMessageW(ctypes.c_void_p(windows[0]), WM_CLOSE, None, None)
            try:
                panel.wait(timeout=10)
            except subprocess.TimeoutExpired:
                panel.kill()
            if tracer is not None:
                try:
                    tracer.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    tracer.kill()
            elif args.client == "tramp":
                # tramp mode: wait for the report to be complete.
                deadline = time.time() + 25
                while time.time() < deadline:
                    if os.path.exists(log_path) and "child exited" in open(
                            log_path, encoding="utf-8", errors="replace").read():
                        break
                    time.sleep(0.2)
            else:
                # hook mode: a report exists only if the daemon crashed; give
                # the dying process a moment to finish writing it.
                time.sleep(4)
            if log is not None:
                log.close()
            text = open(log_path, encoding="utf-8", errors="replace").read() \
                if os.path.exists(log_path) else ""
            if "exception" in text:
                crashes += 1
                print("round %2d: CRASH%s\n%s" % (index, "" if ready else " (panel never got ready)",
                                                  text))
            else:
                tail = [line for line in text.splitlines() if "child exited" in line]
                detail = tail[0] if tail else ("no crash report" if args.client == "hook"
                                               else "no exit record: %r" % text[-120:])
                print("round %2d: %s (%s)" % (index, "ok" if ready else "NOT READY", detail))
            sys.stdout.flush()
    finally:
        if procmon:
            subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Start-Process -FilePath '%s' -Verb RunAs -ArgumentList '/Terminate'" % procmon],
                           capture_output=True, timeout=60)

        def drop(name):
            for _ in range(40):
                try:
                    os.remove(os.path.join(folder, name))
                    return
                except OSError:
                    time.sleep(0.5)

        for stale in ("MoqiDaemon.sym.exe", "MoqiDaemon.real.exe", "crashtrace.exe"):
            drop(stale)
        if args.client in ("tramp", "hook"):
            drop("MoqiDaemon.exe")  # the tramp, or the hook build
            if os.path.exists(backup):
                os.replace(backup, ship)
    print("daemon_crashtrace: %d/%d rounds crashed; traces in %s" % (crashes, args.rounds, args.out))
    return 1 if crashes else 0

if __name__ == "__main__":
    sys.exit(main())
