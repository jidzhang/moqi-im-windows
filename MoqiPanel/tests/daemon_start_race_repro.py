# -*- coding: utf-8 -*-
"""Reproducer for the portable daemon's start-up race.

A ProcMon capture showed the first MoqiDaemon.exe --serve --portable of one
panel launch dying with 0xC0000005 (access violation) 0.26 s after start, in
the engine init thread; the panel silently reconnected to a second one. This
script replays the panel's connect timing in a loop and reports the crash rate:

  - poll the pipe every millisecond and connect the instant it appears
  - fire METHOD_INIT + METHOD_ON_ACTIVATE (+ METHOD_ON_MENU) back to back
  - hold the connection, then disconnect and wait for the daemon's own exit

Variants isolate the trigger:
  --delay N     wait N ms after the pipe appears before connecting
  --no-menu     do not send METHOD_ON_MENU
  --hold N      keep the connection N ms after the last reply (0 = drop at once)

Usage:
  python MoqiPanel/tests/daemon_start_race_repro.py --rounds 25 [--delay 400]
"""
import argparse
import getpass
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from portable_daemon_check import PipeClient  # same key protocol as the panel


def one_round(folder, index, delay_ms, send_menu, hold_ms):
    """Returns (stage, exit status). stage: ok / crash / no-pipe / hung."""
    pipe_name = "\\\\.\\pipe\\%s\\MoqiIM\\RaceRepro.%d.%d" % (getpass.getuser(), os.getpid(), index)
    env = dict(os.environ)
    env["MOQI_SERVE_PIPE"] = pipe_name
    env["MOQI_SERVE_MUTEX"] = "MoqiRaceReproMutex.%d.%d" % (os.getpid(), index)
    proc = subprocess.Popen([os.path.join(folder, "MoqiDaemon.exe"), "--serve", "--portable"],
                            env=env, cwd=folder, creationflags=subprocess.CREATE_NO_WINDOW)
    client = None
    stage = "ok"
    try:
        deadline = time.time() + 20
        while client is None and time.time() < deadline:
            try:
                client = PipeClient(pipe_name)
            except OSError:
                if proc.poll() is not None:
                    return "died-before-connect", proc.returncode
                time.sleep(0.001)
        if client is None:
            return "no-pipe", proc.returncode
        if delay_ms:
            time.sleep(delay_ms / 1000.0)
        client.call("METHOD_INIT", {"guid": "{00000000-0000-0000-0000-000000000001}",
                                    "isWindows8Above": True, "isKeyboardOpen": True,
                                    "isMetroApp": False, "isUiLess": False,
                                    "isConsole": False})
        client.call("METHOD_ON_ACTIVATE", {"isKeyboardOpen": True})
        if send_menu:
            client.call("METHOD_ON_MENU")
        if hold_ms:
            time.sleep(hold_ms / 1000.0)
    except (RuntimeError, OSError) as error:
        stage = "rpc failed: %s" % error
    finally:
        if client is not None:
            client.f.close()
    # 便携 daemon 随末连接断开自退。
    try:
        status = proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        return "hung", None
    if status != 0:
        return "crash", status
    return stage, status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist", "MoqiPanel"))
    parser.add_argument("--rounds", type=int, default=25)
    parser.add_argument("--delay", type=int, default=0)
    parser.add_argument("--no-menu", action="store_true")
    parser.add_argument("--hold", type=int, default=1500)
    args = parser.parse_args()
    folder = os.path.abspath(args.folder)
    print("rounds=%d delay=%dms menu=%s hold=%dms" % (
        args.rounds, args.delay, not args.no_menu, args.hold))

    bad = 0
    for index in range(args.rounds):
        stage, status = one_round(folder, index, args.delay, not args.no_menu, args.hold)
        if stage != "ok":
            bad += 1
            code = "0x%08X" % (status & 0xFFFFFFFF) if isinstance(status, int) else status
            print("round %2d: %s  exit=%s" % (index, stage, code))
    print("daemon_start_race_repro: %d/%d rounds abnormal" % (bad, args.rounds))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
