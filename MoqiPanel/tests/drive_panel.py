# -*- coding: utf-8 -*-
"""Drive the real MoqiPanel.exe through window messages and capture screenshots.

The panel is a normal top-level window, so the driver never injects global
keyboard input: every key is PostMessage'd to the panel's own edit control
(WM_KEYDOWN/WM_KEYUP only -- the panel's message loop deliberately skips
TranslateMessage for those, see PanelActions.cpp "按键闸门"), buttons are
pressed with BM_CLICK, and state is read back with WM_GETTEXT. Nothing can
leak into another window on the desktop.

Scenario (also the acceptance list of the panel):
  1. launch, wait until the engine reports ready
  2. type a sentence by full pinyin (Space commits each word, comma/period
     keys commit punctuation) -> text lands in the edit area
  3. 【翻译并复制】 -> real HTTP call; assert: clipboard == translation with
     English punctuation, edit area cleared, result area filled
  4. type more: one committed word + an unfinished composition -> screenshot
     of "typing in progress" (preedit + candidates + committed text + result)
  5. select text in the result area with mouse messages -> auto-copy
  6. 【恢复】 -> source text comes back
  7. Shift tap -> English mode: letters pass through as typed
  8. close the window -> the panel and its daemon are both gone

Usage:
  python MoqiPanel/tests/drive_panel.py --folder MoqiPanel/dist/MoqiPanel \
      --out MoqiPanel/tests/out

Translation needs a config: the driver writes <folder>\\translate.json from
DEEPSEEK_BASE_URL / DEEPSEEK_API_KEY / DEEPSEEK_MODEL if those are set (the
values are never printed). Without them step 3 is reported as SKIPPED.
The clipboard is overwritten by the test; a text clipboard is restored after.
"""
import argparse
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

from PIL import ImageGrab

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
try:
    user32.SetProcessDPIAware()
except AttributeError:
    pass

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_GETTEXT, WM_GETTEXTLENGTH = 0x000D, 0x000E
WM_CLOSE = 0x0010
WM_LBUTTONDOWN, WM_MOUSEMOVE, WM_LBUTTONUP = 0x0201, 0x0200, 0x0202
BM_CLICK = 0x00F5
CF_UNICODETEXT = 13

ENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
user32.GetDlgCtrlID.argtypes = [ctypes.c_void_p]
user32.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.RECT)]
user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
user32.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
user32.SendMessageW.restype = ctypes.c_ssize_t
# 64 位句柄必须显式声明为指针宽度，否则被截断成 int 后 GlobalLock 拿到空指针。
user32.GetClipboardData.restype = ctypes.c_void_p
user32.GetClipboardData.argtypes = [ctypes.c_uint]
user32.SetClipboardData.restype = ctypes.c_void_p
user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
kernel32.GlobalAlloc.restype = ctypes.c_void_p
kernel32.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]

# control ids shared with src/PanelWindow.h (ControlId enum, kIdMode = 1001)
ID_EDIT, ID_RESULT_SOURCE, ID_RESULT_TRANS, ID_STATUS = 1002, 1010, 1012, 1013
ID_TRANSLATE, ID_RESTORE, ID_SETTINGS = 1003, 1005, 1008

# US scan codes.
SCAN = {"a": 0x1E, "b": 0x30, "c": 0x2E, "d": 0x20, "e": 0x12, "f": 0x21, "g": 0x22,
        "h": 0x23, "i": 0x17, "j": 0x24, "k": 0x25, "l": 0x26, "m": 0x32, "n": 0x31,
        "o": 0x18, "p": 0x19, "q": 0x10, "r": 0x13, "s": 0x1F, "t": 0x14, "u": 0x16,
        "v": 0x2F, "w": 0x11, "x": 0x2D, "y": 0x15, "z": 0x2C}
SPECIAL = {" ": (0x20, 0x39), ",": (0xBC, 0x33), ".": (0xBE, 0x34), "return": (0x0D, 0x1C),
           "escape": (0x1B, 0x01), "shift": (0x10, 0x2A), "back": (0x08, 0x0E)}


def top_windows(pid):
    found = []

    def callback(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid:
            name = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(ctypes.c_void_p(hwnd), name, 128)
            if name.value == "MoqiPanelWindow":
                found.append(hwnd)
        return True

    user32.EnumWindows(ENUMPROC(callback), 0)
    return found


def top_window_of_class(pid, class_name):
    found = []

    def callback(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid:
            name = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(ctypes.c_void_p(hwnd), name, 128)
            if name.value == class_name:
                found.append(hwnd)
        return True

    user32.EnumWindows(ENUMPROC(callback), 0)
    return found[0] if found else None


def child_controls(hwnd):
    found = []

    def callback(child, _):
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(ctypes.c_void_p(child), name, 64)
        found.append((child, name.value, user32.GetDlgCtrlID(ctypes.c_void_p(child))))
        return True

    user32.EnumChildWindows(ctypes.c_void_p(hwnd), ENUMPROC(callback), 0)
    return found


def window_text(hwnd):
    # 跨进程读控件文字必须走 WM_GETTEXT（系统自动跨进程封送缓冲区）；
    # GetWindowTextLength 对别的进程里的 Edit 控件恒返回 0。
    length = user32.SendMessageW(ctypes.c_void_p(hwnd), WM_GETTEXTLENGTH, None, None)
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.SendMessageW(ctypes.c_void_p(hwnd), WM_GETTEXT, length + 1,
                        ctypes.addressof(buffer))
    return buffer.value


def key_param(scan, up):
    return 1 | (scan << 16) | (0xC0000000 if up else 0)


def press(hwnd, name, delay=0.06):
    vk, scan = SPECIAL.get(name) or (ord(name.upper()), SCAN[name])
    user32.PostMessageW(ctypes.c_void_p(hwnd), WM_KEYDOWN, vk, key_param(scan, False))
    time.sleep(delay)
    user32.PostMessageW(ctypes.c_void_p(hwnd), WM_KEYUP, vk, key_param(scan, True))
    time.sleep(delay)


def clipboard_text():
    if not user32.OpenClipboard(None):
        return None
    try:
        handle = user32.GetClipboardData(CF_UNICODETEXT)
        if not handle:
            return None
        locked = kernel32.GlobalLock(ctypes.c_void_p(handle))
        if not locked:
            return None
        try:
            return ctypes.wstring_at(locked)
        finally:
            kernel32.GlobalUnlock(ctypes.c_void_p(handle))
    finally:
        user32.CloseClipboard()


def set_clipboard_text(text):
    data = text.encode("utf-16-le") + b"\x00\x00"
    handle = kernel32.GlobalAlloc(0x0042, len(data))
    if not handle:
        return False
    locked = kernel32.GlobalLock(ctypes.c_void_p(handle))
    if not locked:
        return False
    ctypes.memmove(locked, data, len(data))
    kernel32.GlobalUnlock(ctypes.c_void_p(handle))
    if not user32.OpenClipboard(None):
        return False
    try:
        user32.EmptyClipboard()
        return bool(user32.SetClipboardData(CF_UNICODETEXT, handle))
    finally:
        user32.CloseClipboard()


# Exactly what src/PanelCore.cpp converts (the user's finalized table, 11-27
# plus ¥ 【 】 「 」). A lone "…" or "—" is legitimate English punctuation and is
# kept on purpose, so it is NOT in this list.
CONVERTED_PUNCT = ("，", "。", "、", "；", "：", "？", "！", "（", "）",
                   "“", "”", "‘", "’", "～", "《", "》", "……", "——",
                   "¥", "【", "】", "「", "」")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist", "MoqiPanel"))
    parser.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "out"))
    parser.add_argument("--skip-translate", action="store_true")
    parser.add_argument("--bundled-font", action="store_true",
                        help="run the panel with MOQI_PANEL_FONT=bundled (fallback font path)")
    args = parser.parse_args()
    folder = os.path.abspath(args.folder)
    out_dir = os.path.abspath(args.out)
    if args.bundled_font:
        out_dir = os.path.join(out_dir, "bundled-font")
    os.makedirs(out_dir, exist_ok=True)
    failures = []

    def expect(ok, message):
        print("  [%s] %s" % ("PASS" if ok else "FAIL", message))
        if not ok:
            failures.append(message)

    saved_clipboard = clipboard_text()

    config = None
    if not args.skip_translate and os.environ.get("DEEPSEEK_API_KEY"):
        config = {"base_url": os.environ.get("DEEPSEEK_BASE_URL", ""),
                  "api_key": os.environ["DEEPSEEK_API_KEY"],
                  "model": os.environ.get("DEEPSEEK_MODEL", "")}
        with open(os.path.join(folder, "translate.json"), "w", encoding="utf-8") as fh:
            json.dump(config, fh, indent=2)
        print("translate.json written from DEEPSEEK_* environment (values not shown)")

    # --bundled-font: force the font path a machine WITHOUT Microsoft YaHei
    # takes (the bundled Noto Sans SC), so that fallback can be eyeballed here.
    child_env = dict(os.environ)
    if args.bundled_font:
        child_env["MOQI_PANEL_FONT"] = "bundled"
    proc = subprocess.Popen([os.path.join(folder, "MoqiPanel.exe")], cwd=folder, env=child_env)
    print("panel pid=%d%s" % (proc.pid, " (bundled font)" if args.bundled_font else ""))
    hwnd = None
    deadline = time.time() + 30
    while time.time() < deadline and hwnd is None:
        windows = top_windows(proc.pid)
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

    # The top-level window can be enumerated before its child controls exist:
    # wait until the controls we drive are all there.
    controls = {}
    deadline = time.time() + 10
    while time.time() < deadline:
        controls = {cid: (h, cls) for h, cls, cid in child_controls(hwnd)}
        if ID_EDIT in controls and ID_STATUS in controls and ID_TRANSLATE in controls:
            break
        if proc.poll() is not None:
            print("FATAL: panel exited early rc=%s" % proc.returncode)
            return 2
        time.sleep(0.2)
    edit = controls[ID_EDIT][0]
    result_source = controls[ID_RESULT_SOURCE][0]
    result_trans = controls[ID_RESULT_TRANS][0]
    status = controls[ID_STATUS][0]
    translate_button = controls[ID_TRANSLATE][0]
    restore_button = controls[ID_RESTORE][0]
    print("controls: %s" % sorted((cid, cls) for cid, (h, cls) in controls.items()))

    def wait_status(*needles, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            text = window_text(status)
            if any(n in text for n in needles):
                return text
            time.sleep(0.1)
        return window_text(status)

    def wait_settled(target, timeout=25):
        """Wait until the control's text stops changing (fresh-deploy warmup
        makes the engine answer slowly; the last keys may still be in flight)."""
        last = None
        end = time.time() + timeout
        while time.time() < end:
            text = window_text(target)
            if text == last and text:
                return text
            last = text
            time.sleep(0.3)
        return last

    def shot(name):
        path = os.path.join(out_dir, name)
        ImageGrab.grab(window=hwnd).save(path)
        return path

    try:
        print("== 1. engine ready ==")
        text = wait_status("就绪", "失败", timeout=60)
        expect("就绪" in text, "engine became ready (status: %s)" % text)

        print("== 2. type by full pinyin ==")
        for word in ["jintian", "tianqi", "hen", "hao"]:
            for ch in word:
                press(edit, ch)
            press(edit, " ")
        press(edit, ",")
        for word in ["shihe", "chumen"]:
            for ch in word:
                press(edit, ch)
            press(edit, " ")
        press(edit, ".")
        # The package ships rime/default.custom.yaml: punctuation keys output
        # ASCII even in Chinese mode (see punctuation_live_check.py).
        typed = wait_settled(edit)
        expect(typed == "今天天气很好,适合出门.", "committed text (got %r)" % typed)

        translation = None
        if config is None:
            print("== 3. translate: SKIPPED (no DEEPSEEK_* environment) ==")
        else:
            print("== 3. translate and copy ==")
            user32.SendMessageW(ctypes.c_void_p(translate_button), BM_CLICK, 0, 0)
            text = wait_status("已翻译", "翻译失败", "翻译成功但复制失败", timeout=90)
            expect("已翻译并把译文复制" in text, "translate succeeded (status: %s)" % text)
            translation = window_text(result_trans)
            clip = clipboard_text() or ""
            print("  translation: %s" % translation)
            expect(bool(translation.strip()), "result area shows the translation")
            expect(window_text(result_source) == typed, "result area shows the source")
            expect(window_text(edit) == "", "edit area cleared after translate")
            expect(clip == translation or clip == translation.replace("“", '"').replace("”", '"'),
                   "clipboard holds the translation")
            expect(not any(c in clip for c in CONVERTED_PUNCT),
                   "clipboard has none of the punctuation the table converts")

        print("== 4. typing in progress -> screenshot ==")
        for ch in "mingtian":
            press(edit, ch)
        press(edit, " ")
        for ch in "shangwu":
            press(edit, ch)
        time.sleep(0.8)
        expect(window_text(edit).endswith("明天"),
               "committed word before the open composition (edit: %r)" % window_text(edit))
        print("  screenshot: %s" % shot("panel-typing.png"))
        press(edit, "escape")
        time.sleep(0.4)
        expect(window_text(edit).endswith("明天") and not window_text(edit).endswith("shangwu"),
               "Esc clears the composition, keeps committed text (edit: %r)" % window_text(edit))

        print("== 5. select in result area -> auto copy ==")
        if translation:
            user32.PostMessageW(ctypes.c_void_p(result_trans), WM_LBUTTONDOWN, 1, (8 << 16) | 8)
            time.sleep(0.05)
            user32.PostMessageW(ctypes.c_void_p(result_trans), WM_MOUSEMOVE, 1, (8 << 16) | 60)
            time.sleep(0.05)
            user32.PostMessageW(ctypes.c_void_p(result_trans), WM_LBUTTONUP, 0, (8 << 16) | 60)
            time.sleep(0.5)
            clip = clipboard_text() or ""
            expect(clip != "" and clip in translation,
                   "selecting text in the result area copies it (got %r)" % clip[:40])

        if translation:
            print("== 6. restore ==")
            user32.SendMessageW(ctypes.c_void_p(restore_button), BM_CLICK, 0, 0)
            time.sleep(0.3)
            expect("今天天气很好,适合出门." in window_text(edit),
                   "restore puts the source back (edit: %r)" % window_text(edit))

        print("== 7. Shift toggles English ==")
        press(edit, "shift")
        time.sleep(0.4)
        press(edit, "a")
        press(edit, "b")
        time.sleep(0.4)
        expect(window_text(edit).endswith("ab"), "letters pass through in English mode (edit: %r)"
               % window_text(edit))
        shot("panel-english.png")

        print("== 8. settings dialog ==")
        user32.PostMessageW(ctypes.c_void_p(controls[ID_SETTINGS][0]), BM_CLICK, 0, 0)
        settings = None
        deadline = time.time() + 10
        while time.time() < deadline and settings is None:
            settings = top_window_of_class(proc.pid, "MoqiPanelSettings")
            time.sleep(0.1)
        if settings is None:
            expect(False, "settings dialog opened")
        else:
            time.sleep(0.4)
            fields = [cid for _h, cls, cid in child_controls(settings) if cls == "Edit"]
            expect(len(fields) == 5, "settings dialog has 5 input fields (got %d)" % len(fields))
            path = os.path.join(out_dir, "panel-settings.png")
            ImageGrab.grab(window=settings).save(path)
            print("  screenshot: %s" % path)
            user32.PostMessageW(ctypes.c_void_p(settings), WM_KEYDOWN, 0x1B, 1)  # Esc
            time.sleep(0.5)
            gone = top_window_of_class(proc.pid, "MoqiPanelSettings") is None
            if not gone:  # older builds; still make sure the test can close the panel
                user32.PostMessageW(ctypes.c_void_p(settings), WM_CLOSE, 0, 0)
                time.sleep(0.5)
            expect(gone, "Esc closes the settings dialog")

        print("== 9. close ==")
        user32.PostMessageW(ctypes.c_void_p(hwnd), WM_CLOSE, 0, 0)
        try:
            proc.wait(timeout=10)
            expect(True, "panel exited on WM_CLOSE (rc=%s)" % proc.returncode)
        except subprocess.TimeoutExpired:
            proc.kill()
            expect(False, "panel did not exit on WM_CLOSE")
    finally:
        if proc.poll() is None:
            proc.kill()
        if saved_clipboard is not None:
            set_clipboard_text(saved_clipboard)

    # The daemon leaves with its last client, asynchronously: wait for it so a
    # following rmdir of user\ does not run into files it still holds open.
    time.sleep(1.0)
    deadline = time.time() + 30
    while time.time() < deadline:
        alive = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-CimInstance Win32_Process -Filter \"Name='MoqiDaemon.exe'\" | "
             "Where-Object { $_.ExecutablePath -like '*MoqiPanel*' }).ProcessId"],
            capture_output=True, text=True).stdout.strip()
        if alive == "":
            break
        time.sleep(0.5)

    # daemon must be gone by itself (portable: last client disconnected).
    time.sleep(2.0)
    leftover = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-CimInstance Win32_Process -Filter \"Name='MoqiDaemon.exe'\" | "
         "Where-Object { $_.ExecutablePath -like '*MoqiPanel*' }).ProcessId"],
        capture_output=True, text=True).stdout.strip()
    expect(leftover == "", "no daemon left behind from the panel folder (pid %s)" % leftover)

    print("drive_panel: %s (%d failure(s))" % ("PASS" if not failures else "FAIL", len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
