# -*- coding: utf-8 -*-
"""Portable-daemon acceptance check (MoqiDaemon.exe --serve --portable).

Drives the portable daemon exactly the way MoqiPanel does (same pipe seams,
same key protocol) and locks down the four promises of portable mode:

  1. all writable data lands under <folder>\\user -- nothing is written to
     %LOCALAPPDATA%\\MoqiIM or %APPDATA%\\Moqi;
  2. the process log is not written to the shared system log (checked by the
     daemon's own pid never appearing there);
  3. no toolbar / tray / clipboard-monitor window is created;
  4. the daemon exits by itself once the last client disconnects.

Also exercises the real typing path: full pinyin -> candidates -> Space commit.

Usage:
  python MoqiPanel/tests/portable_daemon_check.py [--folder PATH]

The check starts from a FRESH user dir (it deletes <folder>\\user first), so it
also measures the first-run dictionary deployment. It shares %APPDATA%\\Moqi
with an installed Moqi IME if one is running on this machine -- run it while
that IME is idle, otherwise the snapshot comparison below reports its writes.
"""
import argparse
import ctypes
import getpass
import json
import os
import shutil
import struct
import subprocess
import sys
import time

USER32 = ctypes.windll.user32
KERNEL32 = ctypes.windll.kernel32
ENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
USER32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
USER32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]

# 便携模式不该出现的窗口类（工具栏/托盘/剪贴板监听）。
FORBIDDEN_WINDOW_CLASSES = ("MoqiDaemonToolbar", "MoqiTrayIconWindow", "MoqiClipboardMonitor")


def windows_of_pid(pid):
    found = []

    def callback(hwnd, _):
        owner = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid:
            name = ctypes.create_unicode_buffer(256)
            USER32.GetClassNameW(ctypes.c_void_p(hwnd), name, 256)
            found.append(name.value)
        return True

    USER32.EnumWindows(ENUMPROC(callback), 0)
    return found


def snapshot(root):
    """{相对路径: (mtime, size)}，目录不存在时为空。"""
    out = {}
    if not os.path.isdir(root):
        return out
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            try:
                stat = os.stat(full)
            except OSError:
                continue
            out[os.path.relpath(full, root)] = (stat.st_mtime_ns, stat.st_size)
    return out


def snapshot_diff(before, after):
    changed = []
    for key in sorted(set(before) | set(after)):
        if before.get(key) != after.get(key):
            changed.append(key)
    return changed


class PipeClient:
    def __init__(self, pipe_name):
        self.f = open(pipe_name, "r+b", buffering=0)
        self.seq = 0
        self.last_candidates = []

    def call(self, method, extra=None):
        self.seq += 1
        req = {"method": method, "seqNum": self.seq}
        if extra:
            req.update(extra)
        body = json.dumps(req).encode("utf-8")
        self.f.write(struct.pack("<I", len(body)) + body)
        for _ in range(64):
            hdr = b""
            while len(hdr) < 4:
                chunk = self.f.read(4 - len(hdr))
                if not chunk:
                    raise RuntimeError("pipe closed")
                hdr += chunk
            size = struct.unpack("<I", hdr)[0]
            buf = b""
            while len(buf) < size:
                chunk = self.f.read(size - len(buf))
                if not chunk:
                    raise RuntimeError("pipe closed mid-frame")
                buf += chunk
            msg = json.loads(buf.decode("utf-8"))
            if msg.get("seqNum") == self.seq:
                return msg
        raise RuntimeError("no matching reply for " + method)

    def key(self, vk, ch=None):
        ke = {"charCode": ord(ch) if ch else 0, "keyCode": vk,
              "repeatCount": 1, "scanCode": 0, "isExtended": False}
        commit = ""
        r1 = self.call("METHOD_FILTER_KEY_DOWN", {"keyEvent": ke})
        if r1.get("returnValue"):
            r2 = self.call("METHOD_ON_KEY_DOWN", {"keyEvent": ke})
            commit += r2.get("commitString") or ""
        rk = self.call("METHOD_FILTER_KEY_UP", {"keyEvent": ke})
        if rk.get("returnValue"):
            ru = self.call("METHOD_ON_KEY_UP", {"keyEvent": ke})
            commit += ru.get("commitString") or ""
        entries = r2.get("candidateEntries") if r1.get("returnValue") else None
        if entries is not None:
            self.last_candidates = [e.get("text", "") for e in entries if isinstance(e, dict)]
        return commit


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist", "MoqiPanel"))
    args = parser.parse_args()
    folder = os.path.abspath(args.folder)
    daemon_exe = os.path.join(folder, "MoqiDaemon.exe")
    user_dir = os.path.join(folder, "user")
    failures = []

    def expect(ok, message):
        print("  [%s] %s" % ("PASS" if ok else "FAIL", message))
        if not ok:
            failures.append(message)

    if not os.path.isfile(daemon_exe):
        print("FATAL: %s not found (run package-panel.ps1 first)" % daemon_exe)
        return 2

    # 全新用户目录：首次部署也算进验收。
    if os.path.isdir(user_dir):
        shutil.rmtree(user_dir, ignore_errors=True)
        if os.path.isdir(user_dir):
            print("FATAL: user dir is locked by a running daemon: %s" % user_dir)
            return 2

    local_root = os.path.join(os.environ.get("LOCALAPPDATA", ""), "MoqiIM")
    roaming_root = os.path.join(os.environ.get("APPDATA", ""), "Moqi")
    sys_before = {"local": snapshot(local_root), "roaming": snapshot(roaming_root)}

    pipe_name = "\\\\.\\pipe\\%s\\MoqiIM\\PanelCheck.%d" % (getpass.getuser(), os.getpid())
    env = dict(os.environ)
    env["MOQI_SERVE_PIPE"] = pipe_name
    env["MOQI_SERVE_MUTEX"] = "MoqiPanelCheckMutex.%d" % os.getpid()
    proc = subprocess.Popen([daemon_exe, "--serve", "--portable"], env=env, cwd=folder,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    print("daemon pid=%d folder=%s" % (proc.pid, folder))

    client = None
    try:
        deadline = time.time() + 60
        while time.time() < deadline and client is None:
            try:
                client = PipeClient(pipe_name)
            except OSError:
                if proc.poll() is not None:
                    print("FATAL: daemon exited early rc=%s" % proc.returncode)
                    return 2
                time.sleep(0.2)
        if client is None:
            print("FATAL: pipe not ready in 60s")
            return 2

        client.call("METHOD_INIT", {"guid": "{00000000-0000-0000-0000-000000000001}",
                                    "isWindows8Above": True, "isKeyboardOpen": True,
                                    "isMetroApp": False, "isUiLess": False,
                                    "isConsole": False})
        client.call("METHOD_ON_ACTIVATE", {"isKeyboardOpen": True})

        print("== typing ==")
        t0 = time.time()
        for ch in "nihao":
            client.key(ord(ch.upper()), ch)
        first_key_seconds = time.time() - t0
        first = client.last_candidates[0] if client.last_candidates else "(none)"
        expect(first == "你好", "nihao -> first candidate is 你好 (got %s)" % first)
        print("  first key round-trip incl. first-run deploy: %.1fs" % first_key_seconds)
        commit = client.key(32, " ")
        expect(commit == "你好", "Space commits 你好 (got %r)" % commit)
        expect(not client.last_candidates, "candidates cleared after commit")

        print("== windows ==")
        classes = windows_of_pid(proc.pid)
        leaked = [c for c in classes if c.startswith(FORBIDDEN_WINDOW_CLASSES)]
        expect(not leaked, "no toolbar/tray/clipboard window (classes: %s)" % (classes or "none"))

        print("== data location ==")
        expect(os.path.isdir(os.path.join(user_dir, "MoqiIM", "Log")),
               "engine log dir is <folder>\\user\\MoqiIM\\Log")
        expect(os.path.isdir(os.path.join(user_dir, "Moqi")),
               "config root is <folder>\\user\\Moqi")
        expect(os.path.isdir(os.path.join(user_dir, "build")),
               "rime staging dir is <folder>\\user\\build")
        userdbs = [n for n in os.listdir(user_dir) if n.endswith(".userdb")]
        expect(bool(userdbs), "user dict created under <folder>\\user (%s)" % (userdbs or "none"))
    finally:
        if client is not None:
            client.f.close()

    print("== last client gone ==")
    try:
        proc.wait(timeout=15)
        expect(proc.returncode == 0, "daemon exited by itself after the last client "
              "disconnected (rc=%s)" % proc.returncode)
    except subprocess.TimeoutExpired:
        proc.kill()
        expect(False, "daemon still alive 15s after the last client disconnected")

    print("== system locations untouched ==")
    sys_after = {"local": snapshot(local_root), "roaming": snapshot(roaming_root)}
    shared_log = os.path.join(local_root, "Log", "moqi-daemon.log")
    pid_in_log = False
    try:
        with open(shared_log, "rb") as fh:
            pid_in_log = ("pid=%d" % proc.pid).encode() in fh.read()
    except OSError:
        pass
    expect(not pid_in_log, "this daemon never wrote the shared %LOCALAPPDATA% log")
    expect(not snapshot_diff(sys_before["roaming"], sys_after["roaming"]),
           "%%APPDATA%%\\Moqi unchanged (%s)" % snapshot_diff(sys_before["roaming"], sys_after["roaming"]))
    expect(not snapshot_diff(sys_before["local"], sys_after["local"]),
           "%%LOCALAPPDATA%%\\MoqiIM unchanged (%s)" % snapshot_diff(sys_before["local"], sys_after["local"]))

    print("portable_daemon_check: %s (%d failure(s))" % ("PASS" if not failures else "FAIL", len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
