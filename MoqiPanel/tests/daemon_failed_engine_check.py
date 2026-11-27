# -*- coding: utf-8 -*-
"""Deterministic regression for the daemon's "RPC into an uninitialized
librime" crash.

DaemonMenuService::gather() is reached from ON_ACTIVATE (via
buildActivatePayload) as soon as a client connects -- and it used to call
RimeGetSchemaList whenever the function pointer was non-null. The pointers
turn non-null in loadLibrary(), but librime only registers its components in
RimeSetup()/RimeInitialize() at the END of engine start-up. Any client request
that lands in between dereferences a missing component and kills the daemon
with 0xC0000005.

The window is normally a few milliseconds wide; this check holds it open
without touching the file system: the test process owns the engine's
cross-process start-up mutex (MoqiRimeInProcInit), so the daemon's init thread
loads rime.dll and then blocks. In that state ON_ACTIVATE must answer
normally, not crash. (The same half-initialised state is what a FAILED engine
start-up leaves behind for good.)

Usage:
  python MoqiPanel/tests/daemon_failed_engine_check.py [--folder ...]
"""
import argparse
import ctypes
import getpass
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from portable_daemon_check import PipeClient

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
INIT_REQUEST = {"guid": "{00000000-0000-0000-0000-000000000002}", "isWindows8Above": True,
                "isKeyboardOpen": True, "isMetroApp": False, "isUiLess": False,
                "isConsole": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist", "MoqiPanel"))
    args = parser.parse_args()
    folder = os.path.abspath(args.folder)
    failures = []

    def expect(ok, message):
        print("  [%s] %s" % ("PASS" if ok else "FAIL", message))
        if not ok:
            failures.append(message)

    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    mutex = kernel32.CreateMutexW(None, True, "MoqiRimeInProcInit")  # owned by this process
    already = ctypes.get_last_error() == 183  # ERROR_ALREADY_EXISTS
    expect(bool(mutex) and not already, "test owns the engine start-up mutex")

    pipe_name = "\\\\.\\pipe\\%s\\MoqiIM\\FailedEngine.%d" % (getpass.getuser(), os.getpid())
    env = dict(os.environ)
    env["MOQI_SERVE_PIPE"] = pipe_name
    env["MOQI_SERVE_MUTEX"] = "MoqiFailedEngineMutex.%d" % os.getpid()
    proc = subprocess.Popen([os.path.join(folder, "MoqiDaemon.exe"), "--serve", "--portable"],
                            env=env, cwd=folder, creationflags=subprocess.CREATE_NO_WINDOW)
    client = None
    status = None
    try:
        deadline = time.time() + 30
        while client is None and time.time() < deadline:
            try:
                client = PipeClient(pipe_name)
            except OSError:
                time.sleep(0.02)
        expect(client is not None, "daemon serves its pipe")
        if client is None:
            proc.kill()
            return 1
        try:
            reply = client.call("METHOD_INIT", INIT_REQUEST)
            expect(reply.get("success") is True, "INIT answered")
            # Let the init thread load rime.dll; it then blocks on the mutex.
            time.sleep(2.5)
            reply = client.call("METHOD_ON_ACTIVATE", {"isKeyboardOpen": True})
            expect(reply.get("success") is True,
                   "ON_ACTIVATE answered while librime is loaded but not initialized "
                   "(got %s)" % json.dumps(reply)[:140])
            expect(proc.poll() is None, "daemon still alive after ON_ACTIVATE")
        except (RuntimeError, OSError) as error:
            expect(False, "requests completed (%s)" % error)
    finally:
        if client is not None:
            client.f.close()
        kernel32.ReleaseMutex(mutex)
        kernel32.CloseHandle(mutex)
        try:
            status = proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            status = None
    expect(status == 0, "daemon exited cleanly on last-client-gone (status %s)" % status)
    print("daemon_failed_engine_check: %s" % ("PASS" if not failures else "FAIL"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
