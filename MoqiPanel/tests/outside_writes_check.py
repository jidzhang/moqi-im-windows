# -*- coding: utf-8 -*-
"""""Green" acceptance: prove with Process Monitor that MoqiPanel.exe and the
MoqiDaemon.exe it starts never write outside their own folder.

Steps:
  1. start Procmon (elevated; UAC must not prompt) with a backing file
  2. run drive_panel.py -- the full panel scenario
  3. stop Procmon, export the log to CSV
  4. classify the events of exactly two kinds of process:
       - MoqiPanel.exe with the pid it was started with from <folder>
       - every MoqiDaemon.exe that MoqiPanel.exe created (from the "Process
         Create" events) -- an installed Moqi IME daemon of the same name may
         be running on the machine and must not be counted
     A violation is
       - a SUCCESSFUL write-capable operation on a disk path outside the
         folder (CreateFile with write/delete access, WriteFile, rename,
         delete, set-disposition/end-of-file/basic-information), or
       - a registry key/value create, set or delete.
     Not violations, counted and listed for context:
       - kernel objects (named pipes, AFD sockets, console)
       - CreateDirectory attempts on ancestors that already exist (result
         NAME COLLISION: std::filesystem::create_directories probing each
         level; nothing is created)
       - RegSetInfoKey with KeySetHandleTagsInformation (an in-memory handle
         tag the OS puts on keys it opens; --control shows Notepad does it too)
       - RegCreateKey ending in REG_OPENED_EXISTING_KEY (opening an existing
         key, e.g. crypto opening the system certificate stores)
  5. OS bookkeeping by other processes that mentions the panel (prefetch,
     icon cache) -- reported, not counted.

Usage:
  python MoqiPanel/tests/outside_writes_check.py              # capture + classify
  python MoqiPanel/tests/outside_writes_check.py --reuse-pml  # re-export an existing capture
  python MoqiPanel/tests/outside_writes_check.py --reuse-csv panel.csv
  python MoqiPanel/tests/outside_writes_check.py --control    # Notepad baseline

Every Procmon invocation is elevated; at the lowest UAC level that happens
without a prompt. Procmon loads its kernel driver and unloads it on exit.
"""
import argparse
import collections
import csv
import os
import re
import subprocess
import sys
import time

WRITE_OPERATIONS = {
    "WriteFile", "SetEndOfFileInformationFile", "SetAllocationInformationFile",
    "SetBasicInformationFile", "SetDispositionInformationFile",
    "SetDispositionInformationExFile", "SetRenameInformationFile",
    "SetRenameInformationFileEx", "SetValidDataLengthFile", "SetSecurityFile",
    "SetModeInformationFile", "SetEncryptionFile",
}
WRITE_ACCESS = ("Write Data", "Write Attributes", "Write EA", "Write DAC", "Write Owner",
                "Append Data", "Generic Write", "Generic All", "Maximum Allowed", "Delete")
REGISTRY_VALUE_WRITES = ("RegSetValue", "RegDeleteKey", "RegDeleteValue", "RegFlushKey",
                         "RegRestoreKey", "RegLoadKey", "RegSaveKey", "RegUnLoadKey",
                         "RegSetKeySecurity")
# Not files on disk: named pipes, sockets, console, crypto device.
OBJECT_PREFIXES = ("\\Device\\NamedPipe\\", "\\Device\\Afd", "\\Device\\ConDrv\\",
                   "\\Device\\KsecDD", "\\Device\\Null", "\\Device\\ConIn$", "\\Device\\ConOut$",
                   "\\RPC Control\\", "\\Device\\PsrLookup\\")
# Execution-history bookkeeping the OS keeps for every program it runs.
BAM_KEY_PREFIX = "hklm\\system\\currentcontrolset\\services\\bam\\"
FIELD_ENDINGS = (", Disposition:", ", Options:", ", ShareMode:", ", AllocationSize:",
                 ", SyncType:", ", PageProtection:", ", Attributes:", ", Length:",
                 ", KeySetInformationClass:")


def run_text(command, timeout):
    """Run a console command and never die on its output encoding: cmd and
    PowerShell speak GBK on a Chinese system, utf-8 elsewhere."""
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=timeout)
    return result.returncode, (result.stdout or "") + (result.stderr or "")


def elevated(procmon, arguments):
    """Run Procmon elevated (manifest requireAdministrator) without waiting."""
    return run_text(["powershell", "-NoProfile", "-Command",
                     "Start-Process -FilePath '%s' -Verb RunAs -ArgumentList '%s'" % (procmon, arguments)],
                    60)


def folder_roots(folder):
    """Every spelling Procmon may use for the folder: the given path plus the
    UNC form when the drive is a network mapping (Z:\\x -> \\\\host\\share\\x).
    The share name may contain spaces ("\\\\vmware-host\\Shared Folders"), so
    the remote is taken as everything from \\\\ to the end of the line."""
    folder = folder.lower().rstrip("\\") + "\\"
    roots = [folder]
    drive = os.path.splitdrive(folder)[0]
    if drive:
        _, output = run_text(["net", "use", drive], 30)
        for line in output.splitlines():
            remote = re.search(r"\\\\.*$", line)
            if remote and line.strip() != remote.group(0).strip():
                roots.append(remote.group(0).strip().lower() + folder[len(drive):])
                break
    return roots


def field(detail, name):
    """Value of "name: value" inside a Procmon Detail string."""
    start = detail.find(name + ": ")
    if start < 0:
        return ""
    start += len(name) + 2
    end = len(detail)
    for separator in FIELD_ENDINGS:
        position = detail.find(separator, start)
        if position >= 0:
            end = min(end, position)
    return detail[start:end]


def spawned_pid(detail):
    match = re.search(r"PID:\s*(\d+)", detail)
    return match.group(1) if match else ""


def classify(export, roots, baseline_image=None):
    """Two passes over the CSV. Returns a report dict (see report())."""
    report = {
        "total": 0, "panels": [], "daemons": [], "events": collections.Counter(),
        "inside": collections.Counter(), "outside_ok": collections.OrderedDict(),
        "outside_failed": collections.OrderedDict(), "registry": collections.OrderedDict(),
        "os_bookkeeping": collections.OrderedDict(), "objects": collections.Counter(),
        "noop_creates": collections.OrderedDict(),
        "handle_tags": collections.OrderedDict(), "opened_existing": collections.OrderedDict(),
        "network": collections.Counter(), "exits": [], "other": collections.OrderedDict(),
        "baseline": baseline_image is not None,
    }

    def ours(name, pid):
        return any(pid == p and name == n for n, p in report["panels"] + report["daemons"])

    def inside(path):
        return any(path.lower().startswith(root) for root in roots)

    with open(export, encoding="utf-8-sig", errors="replace", newline="") as fh:
        rows = csv.DictReader(fh)
        for row in rows:
            report["total"] += 1
            name, op = row["Process Name"], row["Operation"]
            path, detail = row["Path"], row["Detail"]
            if op != "Process Create":
                continue
            # A "Process Create" row is attributed to the PARENT; the child's
            # image is in Path and its pid in "PID: n" of Detail.
            child = spawned_pid(detail)
            if not child:
                continue
            lower = path.lower()
            if baseline_image:
                if lower.endswith(baseline_image):
                    report["panels"] = [(os.path.basename(lower), child)]
            elif lower.endswith("moqipanel.exe") and inside(lower):
                report["panels"].append(("MoqiPanel.exe", child))
            elif lower.endswith("moqidaemon.exe") and inside(lower) and \
                    any(row["PID"] == p for _n, p in report["panels"]):
                report["daemons"].append(("MoqiDaemon.exe", child))

    with open(export, encoding="utf-8-sig", errors="replace", newline="") as fh:
        rows = csv.DictReader(fh)
        for row in rows:
            name, pid, op = row["Process Name"], row["PID"], row["Operation"]
            path, status, detail = row["Path"], row["Result"], row["Detail"]
            if not ours(name, pid):
                if op in WRITE_OPERATIONS or (op == "CreateFile" and status == "SUCCESS"
                                              and "write" in field(detail, "Desired Access").lower()):
                    if path and not path.startswith("\\Device\\") and "moqipanel" in path.lower():
                        report["other"]["%s | %s | %s" % (name, op, path)] = 1
                continue
            report["events"][name] += 1
            if op.startswith("TCP") or op.startswith("UDP"):
                report["network"]["%s %s" % (name, op)] += 1
                continue
            if op == "Process Exit":
                report["exits"].append((name, pid, field(detail, "Exit Status") or detail[:60]))
                continue
            if op.startswith("Reg"):
                disposition = field(detail, "Disposition")
                access = field(detail, "Desired Access")
                if "REG_CREATED_NEW_KEY" in disposition or op.startswith(REGISTRY_VALUE_WRITES):
                    if status == "SUCCESS":
                        key = "%s | %s | %s | %s" % (name, op, path, disposition)
                        if path.lower().startswith(BAM_KEY_PREFIX):
                            # Background Activity Moderator: Windows logs program
                            # execution history under HKLM on the process's behalf
                            # (a non-elevated process cannot write there itself).
                            report["os_bookkeeping"][key] = 1
                        else:
                            report["registry"][key] = 1
                elif op == "RegSetInfoKey":
                    report["handle_tags"]["%s | %s | %s" % (
                        name, path, field(detail, "KeySetInformationClass") or detail[:40])] = 1
                elif op.startswith("RegCreateKey") and "REG_OPENED_EXISTING_KEY" in disposition:
                    report["opened_existing"]["%s | %s | %s" % (name, access, path)] = 1
                continue
            write_capable = op in WRITE_OPERATIONS or (
                op == "CreateFile" and re.search(r"\b(Write|Append|Delete|Generic All|Maximum Allowed)",
                                                 field(detail, "Desired Access")))
            if not write_capable:
                continue  # reads outside the folder (system DLLs, fonts) are expected
            if path.startswith(OBJECT_PREFIXES):
                report["objects"]["%s | %s" % (name, op)] += 1
                continue
            if inside(path):
                report["inside"][name] += 1
                continue
            key = "%s | %s | %s | %s" % (name, op, status, path)
            if status == "NAME COLLISION":
                report["noop_creates"]["%s | %s" % (name, path)] = 1
            elif status == "SUCCESS":
                report["outside_ok"][key] = 1
            else:
                report["outside_failed"][key] = 1
    return report


def listing(title, items, limit=30):
    print("%s: %d" % (title, len(items)))
    for key in list(items)[:limit]:
        print("    " + key)
    if len(items) > limit:
        print("    ... %d more" % (len(items) - limit))


def report(result):
    print("events in log: %d" % result["total"])
    print("tracked processes: %s" % ", ".join("%s(%s)" % pair for pair in result["panels"]))
    if not result["panels"]:
        print("  [FAIL] the panel's own process was not found in the capture")
        ok = False
    print("daemons started by the panel: %s" % [pid for _n, pid in result["daemons"]])
    for name, count in sorted(result["events"].items()):
        print("  %-16s %d events" % (name, count))
    print("network (translation): %s" % dict(result["network"]))
    print("")
    ok = True
    for name, pid, status in result["exits"]:
        # "0, User Time: ..." / "-1073741819, User Time: ..." -> leading integer.
        code = re.match(r"-?\d+", status.strip())
        clean = code is not None and int(code.group(0)) == 0
        # The baseline process is terminated by the test itself; its exit
        # code says nothing, so only a crash there is worth flagging.
        expected = clean or (result["baseline"] and not (
            code and int(code.group(0)) not in (0, 1)))
        print("  [%s] %s(%s) exit status %s" % ("PASS" if expected else "FAIL", name, pid,
                                               code.group(0) if code else status))
        ok = ok and expected
    if not result["baseline"] and result["panels"] and len(result["daemons"]) > 1:
        print("  [FAIL] the panel had to start more than one daemon (a daemon died early)")
        ok = False
    for title, items in (("file writes OUTSIDE the folder that SUCCEEDED", result["outside_ok"]),
                         ("file writes outside the folder that FAILED (attempted)", result["outside_failed"]),
                         ("registry keys/values created, set or deleted", result["registry"])):
        listing(title, items)
        if items:
            ok = False
    print("")
    print("writes inside the folder: %s" % dict(result["inside"]))
    print("network: %s" % dict(result["network"]))
    print("object-manager paths used as files: %s" % dict(result["objects"]))
    listing("registry writes by the OS on the process's behalf (Background Activity Moderator)",
            result["os_bookkeeping"], 6)
    listing("OS handle tags on registry keys (KeySetHandleTagsInformation, not stored)", result["handle_tags"], 12)
    listing("existing registry keys opened (REG_OPENED_EXISTING_KEY)", result["opened_existing"], 12)
    listing("create attempts on directories that already exist (no-op)", result["noop_creates"], 12)
    listing("writes by OTHER processes mentioning the panel (OS bookkeeping, not counted)", result["other"], 12)
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--procmon", default=r"C:\Program Files\coreutils\bin\Procmon64.exe")
    parser.add_argument("--folder", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist", "MoqiPanel"))
    parser.add_argument("--workdir", default=os.path.join(
        os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "Temp", "moqi-panel-procmon"))
    parser.add_argument("--reuse-csv", default="")
    parser.add_argument("--reuse-pml", action="store_true",
                        help="skip capture: export and classify the existing backing file")
    parser.add_argument("--control", action="store_true",
                        help="baseline: capture Notepad instead of the panel")
    args = parser.parse_args()

    folder = os.path.abspath(args.folder)
    roots = folder_roots(folder)
    workdir = os.path.abspath(args.workdir)
    os.makedirs(workdir, exist_ok=True)
    backing = os.path.join(workdir, "panel.pml")
    export = args.reuse_csv or os.path.join(workdir, "panel.csv")
    for stale in ([] if args.reuse_pml else [backing]) + ([] if args.reuse_csv else [export]):
        if os.path.exists(stale):
            os.remove(stale)
    print("folder roots: %s" % roots)

    reuse_only = bool(args.reuse_pml or args.reuse_csv)
    if not reuse_only:
        print("== start Procmon ==")
        code, output = elevated(args.procmon, "/AcceptEula /Quiet /Minimized /BackingFile %s" % backing)
        if code != 0:
            print("FATAL: could not start Procmon: %s" % output.strip()[:300])
            return 2
        deadline = time.time() + 30
        while time.time() < deadline and not os.path.exists(backing):
            time.sleep(0.5)
        if not os.path.exists(backing):
            print("FATAL: Procmon backing file never appeared")
            return 2
        time.sleep(3)  # let the driver settle

        if args.control:
            print("== control: launch Notepad ==")
            notepad = subprocess.Popen([os.path.join(os.environ["SystemRoot"], "System32", "notepad.exe")])
            time.sleep(6)
            notepad.terminate()
            notepad.wait(timeout=10)
        else:
            print("== drive the panel ==")
            driver = subprocess.run([sys.executable, os.path.join(os.path.dirname(
                os.path.abspath(__file__)), "drive_panel.py"), "--folder", folder,
                "--out", os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=400)
            print(driver.stdout.rstrip())
            if driver.returncode != 0:
                print("note: drive_panel.py exit code %d" % driver.returncode)

        print("== stop Procmon ==")
        elevated(args.procmon, "/Terminate")
        deadline = time.time() + 60
        while time.time() < deadline:
            _, probe = run_text(["tasklist", "/FI", "IMAGENAME eq Procmon64.exe"], 30)
            if "Procmon64.exe" not in probe:
                break
            time.sleep(1.0)

    if not args.reuse_csv:
        print("== export CSV ==")
        # /OpenLog + /SaveAs saves and exits by itself.
        code, output = run_text(["powershell", "-NoProfile", "-Command",
                                 "$p = Start-Process -FilePath '%s' -Verb RunAs -Wait -PassThru "
                                 "-ArgumentList '/OpenLog %s /SaveAs %s'; exit $p.ExitCode"
                                 % (args.procmon, backing, export)], 1800)
        if not os.path.exists(export):
            print("FATAL: export failed (rc=%s) %s" % (code, output.strip()[:300]))
            return 2

    print("== classify ==")
    result = classify(export, roots, baseline_image="notepad.exe" if args.control else None)
    if args.control:
        print("NOTEPAD BASELINE (same machine, same Procmon):")
    ok = report(result)
    print("outside_writes_check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
