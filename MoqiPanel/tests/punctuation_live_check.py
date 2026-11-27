# -*- coding: utf-8 -*-
"""Punctuation in the real MoqiPanel.exe: typed keys and the clipboard.

Two layers, both checked against the shipped folder:

  1. The panel package ships rime/default.custom.yaml, which makes every
     punctuation key output half-width ASCII in Chinese mode too (data only;
     the product's own switching logic is untouched). Typed punctuation is
     therefore English from the start -- also while composing (hao + "," ->
     "好,") and after Shift round trips.
  2. Chinese punctuation that is already in the edit area (pasted from
     elsewhere) is converted when copied. Every copy path is driven: 【复制
     原文】, Ctrl+A, mouse drag select and Ctrl+C.
  3. The patch is not part of librime's deploy dirty check, so changing it
     (new package copied over an old folder, or the patch deleted to get
     Chinese punctuation back) must still redeploy: the panel stamps the
     patch and drops user\build when the stamp changes.

Keys are only PostMessage'd to the panel's own edit control (same as
drive_panel.py). Shifted keys (Shift+1 -> "!") additionally need the panel
thread to see Shift as held, which a posted message cannot do: the driver
therefore attaches its input queue to the panel thread and calls
SetKeyboardState. Nothing is injected into the desktop. If the attach is
refused, shifted keys are reported as SKIPPED and the run fails instead of
passing silently.

Usage:
  python MoqiPanel/tests/punctuation_live_check.py [--folder ...]

The clipboard is overwritten by the test; a text clipboard is restored after.
"""
import argparse
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive_panel as dp  # noqa: E402  (window helpers + ctypes prototypes)

ID_COPY_SOURCE = 1004
WM_SETTEXT = 0x000C
VK_SHIFT, VK_CONTROL = 0x10, 0x11

# Mirrors kPunctRules in src/PanelCore.cpp, same order.
RULES = [
    ("……", "..."), ("——", "-"),
    ("，", ","), ("。", "."), ("、", ","), ("；", ";"), ("：", ":"), ("？", "?"),
    ("！", "!"), ("（", "("), ("）", ")"), ("“", '"'), ("”", '"'), ("‘", "'"),
    ("’", "'"), ("～", "~"), ("《", '"'), ("》", '"'),
    ("¥", "$"), ("【", "["), ("】", "]"), ("「", "{"), ("」", "}"),
]

# (key, virtual key, US scan code, shift) -> the US-layout character of the key.
PUNCT_KEYS = [
    ("`", 0xC0, 0x29, False), ("~", 0xC0, 0x29, True),
    ("-", 0xBD, 0x0C, False), ("_", 0xBD, 0x0C, True),
    ("=", 0xBB, 0x0D, False), ("+", 0xBB, 0x0D, True),
    ("[", 0xDB, 0x1A, False), ("{", 0xDB, 0x1A, True),
    ("]", 0xDD, 0x1B, False), ("}", 0xDD, 0x1B, True),
    ("\\", 0xDC, 0x2B, False), ("|", 0xDC, 0x2B, True),
    (";", 0xBA, 0x27, False), (":", 0xBA, 0x27, True),
    ("'", 0xDE, 0x28, False), ('"', 0xDE, 0x28, True),
    (",", 0xBC, 0x33, False), ("<", 0xBC, 0x33, True),
    (".", 0xBE, 0x34, False), (">", 0xBE, 0x34, True),
    ("/", 0xBF, 0x35, False), ("?", 0xBF, 0x35, True),
    ("!", 0x31, 0x02, True), ("@", 0x32, 0x03, True), ("#", 0x33, 0x04, True),
    ("$", 0x34, 0x05, True), ("%", 0x35, 0x06, True), ("^", 0x36, 0x07, True),
    ("&", 0x37, 0x08, True), ("*", 0x38, 0x09, True),
    ("(", 0x39, 0x0A, True), (")", 0x30, 0x0B, True),
]
# Chinese punctuation for every rule, as if pasted from another program.
PASTED = ("你好，世界。你好、世界；你好：世界？你好！世界（你好）“你好”‘你好’"
          "《你好》～¥100【含税】「注」……——")


def convert(text):
    out = []
    i = 0
    while i < len(text):
        for src, dst in RULES:
            if text.startswith(src, i):
                out.append(dst)
                i += len(src)
                break
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def set_text(edit, text):
    dp.user32.SendMessageW(ctypes.c_void_p(edit), WM_SETTEXT, 0,
                           ctypes.cast(ctypes.c_wchar_p(text), ctypes.c_void_p))


def set_key_state(shift=False, ctrl=False):
    states = (ctypes.c_ubyte * 256)()
    if shift:
        states[VK_SHIFT] = 0x80
    if ctrl:
        states[VK_CONTROL] = 0x80
    dp.user32.SetKeyboardState.argtypes = [ctypes.c_void_p]
    dp.user32.SetKeyboardState.restype = ctypes.c_bool
    return bool(dp.user32.SetKeyboardState(ctypes.cast(states, ctypes.c_void_p)))


def press(edit, vk, scan, shift=False, ctrl=False, delay=0.06):
    # The panel reads the thread's keyboard state when the key message arrives
    # (PanelActions.cpp onEditKeyDown), so the modifier lives in that table --
    # no VK_SHIFT / VK_CONTROL message is posted (a posted Shift tap would make
    # the engine switch to English).
    set_key_state(shift, ctrl)
    dp.user32.PostMessageW(ctypes.c_void_p(edit), dp.WM_KEYDOWN, vk,
                           dp.key_param(scan, False))
    time.sleep(delay)
    dp.user32.PostMessageW(ctypes.c_void_p(edit), dp.WM_KEYUP, vk,
                           dp.key_param(scan, True))
    time.sleep(delay)
    set_key_state()


def type_pinyin(edit, word):
    for ch in word:
        dp.press(edit, ch)


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

    patch = os.path.join(folder, "moqi-ime", "input_methods", "rime", "data",
                         "default.custom.yaml")
    expect(os.path.isfile(patch), "panel package ships data\\default.custom.yaml")

    saved_clipboard = dp.clipboard_text()
    proc = subprocess.Popen([os.path.join(folder, "MoqiPanel.exe")], cwd=folder)
    print("panel pid=%d" % proc.pid)
    hwnd = None
    deadline = time.time() + 30
    while time.time() < deadline and hwnd is None:
        windows = dp.top_windows(proc.pid)
        if windows:
            hwnd = windows[0]
        elif proc.poll() is not None:
            print("FATAL: panel exited early rc=%s" % proc.returncode)
            return 2
        time.sleep(0.2)
    if hwnd is None:
        proc.kill()
        print("FATAL: panel window not found")
        return 2

    controls = {cid: h for h, cls, cid in dp.child_controls(hwnd)}
    edit, status = controls[dp.ID_EDIT], controls[dp.ID_STATUS]
    copy_button = controls[ID_COPY_SOURCE]

    # Share the input state with the panel thread so SetKeyboardState here is
    # what GetKeyboardState there returns.
    msg = wintypes.MSG()
    dp.user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 0)  # give this thread a queue
    dp.user32.AttachThreadInput.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_bool]
    dp.user32.AttachThreadInput.restype = ctypes.c_bool
    panel_thread = dp.user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), None)
    my_thread = dp.kernel32.GetCurrentThreadId()
    attached = bool(dp.user32.AttachThreadInput(my_thread, panel_thread, True))

    def text():
        return dp.window_text(edit)

    def wait_status(needle, timeout=8.0):
        deadline = time.time() + timeout
        current = dp.window_text(status)
        while needle not in current and time.time() < deadline:
            time.sleep(0.05)
            current = dp.window_text(status)
        return current

    def type_key(vk, scan, shift=False, ctrl=False):
        """Press one key and wait for the edit area to change."""
        before = len(text())
        press(edit, vk, scan, shift=shift, ctrl=ctrl)
        deadline = time.time() + 5.0
        while len(text()) <= before and time.time() < deadline:
            time.sleep(0.05)
        return text()

    def click_copy_source():
        dp.set_clipboard_text("sentinel")
        dp.user32.SendMessageW(ctypes.c_void_p(copy_button), dp.BM_CLICK, 0, 0)
        time.sleep(0.1)
        return dp.clipboard_text() or ""

    def drag_select_all():
        rect = wintypes.RECT()
        dp.user32.GetClientRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.RECT)]
        dp.user32.GetClientRect(ctypes.c_void_p(edit), ctypes.byref(rect))
        width, height = rect.right, rect.bottom

        def at(x, y):
            return (y << 16) | x

        dp.user32.PostMessageW(ctypes.c_void_p(edit), dp.WM_LBUTTONDOWN, 1, at(3, 8))
        time.sleep(0.05)
        for step in range(1, 9):
            dp.user32.PostMessageW(ctypes.c_void_p(edit), dp.WM_MOUSEMOVE, 1,
                                   at(3 + (width - 8) * step // 8, 8))
            time.sleep(0.03)
        dp.user32.PostMessageW(ctypes.c_void_p(edit), dp.WM_MOUSEMOVE, 1,
                               at(width - 5, height - 8))
        time.sleep(0.05)
        dp.user32.PostMessageW(ctypes.c_void_p(edit), dp.WM_LBUTTONUP, 0,
                               at(width - 5, height - 8))
        wait_status("Copied selection")

    def sentinel():
        dp.set_clipboard_text("sentinel")

    try:
        current = wait_status("ready", timeout=60)
        expect("ready" in current, "engine became ready (status: %s)" % current)

        # Probe: the panel must see Shift as held for a posted Shift+1.
        set_text(edit, "")
        usable = attached and type_key(0x31, 0x02, shift=True) == "!"
        if not attached:
            print("  [INFO] could not attach to the panel's input queue; "
                  "Shift+key cases are SKIPPED")
        else:
            expect(usable, "panel sees the held Shift (Shift+1 gives %r)" % text())

        print("== 1. every punctuation key types its ASCII character ==")
        bad = []
        for key, vk, scan, shift in PUNCT_KEYS:
            if shift and not usable:
                failures.append("SKIPPED Shift+%s" % key)
                continue
            set_text(edit, "")
            shown = type_key(vk, scan, shift=shift)
            if shown != key:
                bad.append("%s%s->%r" % ("Shift+" if shift else "", key, shown))
        expect(not bad, "%d keys typed, all ASCII%s" % (
            len(PUNCT_KEYS), "" if not bad else " (wrong: %s)" % ", ".join(bad)))

        print("== 2. punctuation while composing confirms the word ==")
        for word, key in (("hao", ","), ("nihao", "."), ("hao", "!")):
            label, vk, scan, shift = next(k for k in PUNCT_KEYS if k[0] == key)
            if shift and not usable:
                failures.append("SKIPPED composing + Shift+%s" % key)
                continue
            set_text(edit, "")
            type_pinyin(edit, word)
            time.sleep(0.4)
            shown = type_key(vk, scan, shift=shift)
            expect(shown == word.replace("hao", "好").replace("ni", "你") + key,
                   "pinyin %r + %s -> %r" % (word, key, shown))

        print("== 3. Shift round trip keeps English punctuation ==")
        if usable:
            set_text(edit, "")
            dp.press(edit, "shift")
            time.sleep(0.4)
            type_pinyin(edit, "ab")
            shown = type_key(0xBC, 0x33)  # , in English mode
            expect(shown == "ab,", "English mode: a b , -> %r" % shown)
            dp.press(edit, "shift")
            time.sleep(0.4)
            set_text(edit, "")
            type_pinyin(edit, "hao")
            time.sleep(0.4)
            shown = type_key(0xBC, 0x33)
            expect(shown == "好,", "back in Chinese mode: hao , -> %r" % shown)
        else:
            failures.append("SKIPPED Shift round trip")

        print("== 4. Chinese punctuation already in the edit area (pasted) ==")
        set_text(edit, PASTED)
        want = convert(PASTED)
        print("   pasted : %s" % PASTED)
        print("   copied : %s" % want)
        expect(want != PASTED and not any(src in want for src, _ in RULES),
               "every rule changes something (%d rules)" % len(RULES))
        expect(click_copy_source() == want, "Copy Text converts")
        if usable:
            sentinel()
            press(edit, 0x41, 0x1E, ctrl=True)  # Ctrl+A: panel hotkey, select all + copy
            wait_status("Copied selection")
            expect(dp.clipboard_text() == want, "Ctrl+A converts")
        sentinel()
        drag_select_all()
        expect(dp.clipboard_text() == want, "mouse drag select converts")
        if usable:
            sentinel()
            press(edit, 0x43, 0x2E, ctrl=True)  # Ctrl+C goes through the WM_COPY hook
            time.sleep(0.5)
            expect(dp.clipboard_text() == want, "Ctrl+C converts")

        print("== INFO: Ctrl+Shift+3 (the engine's own ascii_punct toggle) ==")
        if usable:
            set_text(edit, "")
            press(edit, 0x33, 0x04, shift=True, ctrl=True)
            time.sleep(0.3)
            type_pinyin(edit, "hao")
            time.sleep(0.4)
            shown = type_key(0xBC, 0x33)
            print("   after the toggle: hao , -> %r" % shown)
            set_text(edit, "")
            press(edit, 0x33, 0x04, shift=True, ctrl=True)  # toggle back
            time.sleep(0.3)
    finally:
        if attached:
            set_key_state()
            dp.user32.AttachThreadInput(my_thread, panel_thread, False)
        if proc.poll() is None:
            dp.user32.PostMessageW(ctypes.c_void_p(hwnd), dp.WM_CLOSE, 0, 0)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if saved_clipboard is not None:
            dp.set_clipboard_text(saved_clipboard)

    print("== 5. data patch changed -> user\\build is deployed again ==")
    # librime's dirty check does not watch a *.custom.yaml in the shared data
    # dir, so the panel keeps a stamp of the patch and drops the regenerable
    # user\build when it changes (EngineClient::refreshDataPatch).
    def daemon_gone():
        return subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-CimInstance Win32_Process -Filter \"Name='MoqiDaemon.exe'\" | "
             "Where-Object { $_.ExecutablePath -like '*MoqiPanel*' }).ProcessId"],
            capture_output=True, text=True).stdout.strip() == ""

    def wait_daemon_gone():
        # The panel never touches user\build while a daemon of this folder is
        # still alive (even exiting), so give it time to leave before the next
        # launch -- otherwise the case under test is the "reuse" path.
        deadline = time.time() + 30
        while not daemon_gone() and time.time() < deadline:
            time.sleep(0.3)

    def launch_and_type_comma():
        again = subprocess.Popen([os.path.join(folder, "MoqiPanel.exe")], cwd=folder)
        window = None
        try:
            deadline = time.time() + 30
            while window is None and time.time() < deadline:
                windows = dp.top_windows(again.pid)
                window = windows[0] if windows else None
                time.sleep(0.2)
            found = {cid: h for h, cls, cid in dp.child_controls(window)}
            deadline = time.time() + 120
            while "ready" not in dp.window_text(found[dp.ID_STATUS]) and time.time() < deadline:
                time.sleep(0.2)
            dp.press(found[dp.ID_EDIT], ",")
            time.sleep(0.6)
            return dp.window_text(found[dp.ID_EDIT])
        finally:
            if again.poll() is None:
                if window is not None:
                    dp.user32.PostMessageW(ctypes.c_void_p(window), dp.WM_CLOSE, 0, 0)
                try:
                    again.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    again.kill()
            wait_daemon_gone()

    parked = patch + ".off"
    wait_daemon_gone()
    try:
        os.rename(patch, parked)
        shown = launch_and_type_comma()
        expect(shown == "，", "patch removed, user\\build left in place -> %r" % shown)
        os.rename(parked, patch)
        shown = launch_and_type_comma()
        expect(shown == ",", "patch back over that old build (in-place upgrade) -> %r" % shown)
    finally:
        if os.path.exists(parked):
            os.rename(parked, patch)

    time.sleep(2.0)
    leftover = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-CimInstance Win32_Process -Filter \"Name='MoqiDaemon.exe'\" | "
         "Where-Object { $_.ExecutablePath -like '*MoqiPanel*' }).ProcessId"],
        capture_output=True, text=True).stdout.strip()
    expect(leftover == "", "no daemon left behind from the panel folder (pid %s)" % leftover)

    print("punctuation_live_check: %s (%d failure(s))" % (
        "PASS" if not failures else "FAIL", len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
