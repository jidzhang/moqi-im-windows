# -*- coding: utf-8 -*-
"""Startup guards: the panel must refuse to start, with a Chinese explanation,
when (a) its folder path contains non-ASCII characters or (b) the folder is
incomplete. Each guard is checked by launching a copy of MoqiPanel.exe placed
in the offending folder, reading the message box, and closing it.

Usage:
  python MoqiPanel/tests/startup_checks.py [--folder MoqiPanel/dist/MoqiPanel]
"""
import argparse
import ctypes
import os
import shutil
import subprocess
import sys
import time

user32 = ctypes.windll.user32
WM_GETTEXT, WM_GETTEXTLENGTH, WM_CLOSE = 0x000D, 0x000E, 0x0010
ENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]


def text_of(hwnd):
    length = user32.SendMessageW(ctypes.c_void_p(hwnd), WM_GETTEXTLENGTH, None, None)
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.SendMessageW(ctypes.c_void_p(hwnd), WM_GETTEXT, length + 1, ctypes.addressof(buffer))
    return buffer.value


def dialog_of(pid, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = []

        def callback(hwnd, _):
            owner = ctypes.c_ulong()
            user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
            if owner.value == pid:
                name = ctypes.create_unicode_buffer(64)
                user32.GetClassNameW(ctypes.c_void_p(hwnd), name, 64)
                if name.value == "#32770":
                    found.append(hwnd)
            return True

        user32.EnumWindows(ENUMPROC(callback), 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def child_text(hwnd):
    parts = []

    def callback(child, _):
        parts.append(text_of(child))
        return True

    user32.EnumChildWindows(ctypes.c_void_p(hwnd), ENUMPROC(callback), 0)
    return " ".join(p for p in parts if p)


def run_guard(exe, expected_fragment):
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe))
    try:
        hwnd = dialog_of(proc.pid, 15)
        if hwnd is None:
            return False, "no message box appeared (rc=%s)" % proc.poll()
        text = child_text(hwnd)
        user32.PostMessageW(ctypes.c_void_p(hwnd), WM_CLOSE, None, None)
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = "still running"
        return expected_fragment in text and rc == 2, "rc=%s text=%r" % (rc, text[:160])
    finally:
        if proc.poll() is None:
            proc.kill()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist", "MoqiPanel"))
    args = parser.parse_args()
    source = os.path.join(os.path.abspath(args.folder), "MoqiPanel.exe")
    root = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "Temp",
                        "moqi-panel-startup-checks")
    shutil.rmtree(root, ignore_errors=True)
    failures = 0

    cases = [
        (os.path.join(root, "墨奇输入板 空格也能有"), "must contain only ASCII letters",
         "non-ASCII folder path is refused"),
        (os.path.join(root, "incomplete"), "folder is incomplete",
         "incomplete folder is refused (only the exe is present)"),
    ]
    for folder, fragment, title in cases:
        os.makedirs(folder)
        exe = os.path.join(folder, "MoqiPanel.exe")
        shutil.copyfile(source, exe)
        ok, detail = run_guard(exe, fragment)
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", title, detail))
        if not ok:
            failures += 1
    shutil.rmtree(root, ignore_errors=True)
    print("startup_checks: %s" % ("PASS" if not failures else "FAIL"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
