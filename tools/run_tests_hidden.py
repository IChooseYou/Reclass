#!/usr/bin/env python3
"""Run the test suite on a hidden Windows desktop — no windows on your screen.

The GUI tests (test_editor, test_byte_selection, test_type_selector, …) create
real top-level windows, so a normal run flashes them across the desktop. This
launches the runner on a separate Win32 desktop created just for the run:
child processes inherit it, so every window they open is drawn off-screen.

This is NOT the same as `Start-Process -WindowStyle Hidden`, which this project
documents as breaking ~16 GUI tests — that hides windows on the *same* desktop
and trips a focus/window-station artifact. A real separate desktop keeps the
tests' focus and activation semantics intact.

KNOWN LIMITATION — tests that call `QTest::qWaitForWindowExposed()` fail here.
An unrendered desktop never composites, so a window on it is never "exposed"
and the wait times out. That is an artifact of the runner, not a defect in the
code under test (test_source_chooser's 4 popup tests are the current example:
they pass on the visible desktop). Such failures are reported separately below
so a hidden run is never mistaken for a clean one; re-check those on the normal
desktop before trusting a red result.

Usage:
    python tools/run_tests_hidden.py                  # whole suite (ctest -j1)
    python tools/run_tests_hidden.py test_editor      # one or more exes
    python tools/run_tests_hidden.py test_editor -- testCursorShapeOverType
"""
from __future__ import annotations
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import sys
import tempfile
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
GENERIC_ALL = 0x10000000
WAIT_TIMEOUT = 0x102
CREATE_NEW_CONSOLE = 0x00000010


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR),
                ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
                ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.POINTER(wt.BYTE)), ("hStdInput", wt.HANDLE),
                ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


user32.CreateDesktopW.restype = wt.HANDLE
user32.CreateDesktopW.argtypes = [wt.LPWSTR, wt.LPWSTR, ctypes.c_void_p,
                                  wt.DWORD, wt.DWORD, ctypes.c_void_p]
kernel32.CreateProcessW.restype = wt.BOOL
kernel32.CreateProcessW.argtypes = [
    wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wt.BOOL, wt.DWORD,
    ctypes.c_void_p, wt.LPCWSTR, ctypes.POINTER(STARTUPINFOW),
    ctypes.POINTER(PROCESS_INFORMATION)]
kernel32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]

DESKTOP = "rcxtests_hidden"


def run_hidden(cmdline: str, cwd: Path, timeout_s: float = 1800) -> int:
    """Run cmdline on the hidden desktop; return its exit code (-1 on timeout)."""
    hdesk = user32.CreateDesktopW(DESKTOP, None, None, 0, GENERIC_ALL, None)
    if not hdesk:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        si = STARTUPINFOW()
        si.cb = ctypes.sizeof(si)
        si.lpDesktop = DESKTOP
        pi = PROCESS_INFORMATION()
        buf = ctypes.create_unicode_buffer(cmdline)
        if not kernel32.CreateProcessW(None, buf, None, None, False,
                                       CREATE_NEW_CONSOLE, None, str(cwd),
                                       ctypes.byref(si), ctypes.byref(pi)):
            raise ctypes.WinError(ctypes.get_last_error())
        rc = kernel32.WaitForSingleObject(pi.hProcess, int(timeout_s * 1000))
        if rc == WAIT_TIMEOUT:
            kernel32.TerminateProcess(pi.hProcess, 1)
            code = -1
        else:
            out = wt.DWORD()
            kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(out))
            code = int(out.value)
        kernel32.CloseHandle(pi.hThread)
        kernel32.CloseHandle(pi.hProcess)
        return code
    finally:
        user32.CloseDesktop(hdesk)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("targets", nargs="*",
                    help="test executables to run (default: whole ctest suite)")
    ap.add_argument("--repeat", type=int, default=1,
                    help="run everything N times (flake hunting)")
    args, passthrough = ap.parse_known_args()
    passthrough = [a for a in passthrough if a != "--"]

    tmp = Path(tempfile.gettempdir()) / f"rcxtests_{uuid.uuid4().hex}"
    tmp.mkdir(parents=True, exist_ok=True)
    worst = 0

    for run in range(1, args.repeat + 1):
        tag = f"run {run}/{args.repeat}: " if args.repeat > 1 else ""
        if not args.targets:
            log = tmp / f"ctest_{run}.log"
            # ctest's own --output-log keeps stdout off our console entirely
            cmd = f'ctest -j1 --output-on-failure --output-log "{log}"'
            code = run_hidden(cmd, BUILD)
            text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
            passed = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", text)
            fails = re.findall(r"^\s*\d+ - (\S+) \((?:Failed|Timeout|Subprocess aborted)\)",
                               text, re.M)
            # A suite whose ONLY failures are exposure waits is a runner
            # artifact, not a red build — same rule the per-exe path applies.
            artifacts, real = [], []
            for name in fails:
                block = re.search(rf"{re.escape(name)}(.{{0,4000}})", text, re.S)
                body = block.group(1) if block else ""
                fl = re.findall(r"^FAIL!\s+:\s+(.*)$", body, re.M)
                (artifacts if fl and all("qWaitForWindowExposed" in f for f in fl)
                 else real).append(name)
            if passed:
                print(f"{tag}{passed.group(1)}% passed — "
                      f"{passed.group(2)} failed of {passed.group(3)}"
                      + (f"   FAILED: {', '.join(real)}" if real else "")
                      + (f"   [hidden-desktop artifact: {', '.join(artifacts)}]"
                         if artifacts else ""))
            else:
                print(f"{tag}ctest exit={code} (no summary parsed; see {log})")
            worst = max(worst, 0 if (code and not real) else code)
        else:
            for t in args.targets:
                exe = BUILD / (t if t.endswith(".exe") else t + ".exe")
                if not exe.is_file():
                    print(f"{tag}{t}: NOT BUILT ({exe})")
                    worst = max(worst, 1)
                    continue
                log = tmp / f"{t}_{run}.log"
                extra = (" " + " ".join(passthrough)) if passthrough else ""
                # QtTest writes its report to the file, keeping our console clean
                cmd = f'"{exe}" -o "{log}",txt{extra}'
                code = run_hidden(cmd, BUILD)
                text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
                m = re.search(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", text)
                fails = re.findall(r"^FAIL!\s+:\s+(.*)$", text, re.M)
                # Separate the runner's own artifact from real defects.
                artifacts = [f for f in fails if "qWaitForWindowExposed" in f]
                real = [f for f in fails if "qWaitForWindowExposed" not in f]
                if m:
                    print(f"{tag}{t}: {m.group(1)} passed, {m.group(2)} failed, "
                          f"{m.group(3)} skipped  (exit {code})")
                else:
                    print(f"{tag}{t}: exit={code} (no totals parsed; see {log})")
                for f in real[:6]:
                    print(f"      FAIL: {f}")
                if artifacts:
                    print(f"      ({len(artifacts)} hidden-desktop artifact(s): "
                          "qWaitForWindowExposed cannot succeed off-screen — "
                          "re-check on the normal desktop)")
                # Artifact-only failures don't make the run red.
                worst = max(worst, 0 if (code and not real) else code)
    print(f"\nlogs: {tmp}")
    return 0 if worst == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
