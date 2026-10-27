#!/usr/bin/env python3
"""Process control of the test network's nodes on Windows (Git Bash), used by run.sh there only.

Usage: winproc.py run <pidfile> <program> [args...]
       winproc.py stop <windows pid>

run: starts the program in a console of its own (hidden), with this process's standard handles,
writes its Windows pid to <pidfile>, waits for it and exits with its exit code. The program is put in
a job object that ends it when this process ends, so a hard kill of this process (kill -KILL from
Git Bash) ends the node as well.

stop: the graceful stop of such a program, as SIGTERM on Linux: Ctrl-C on its console (the node's
console handler requests the same stop as its SIGTERM path). A signal sent from Git Bash to a native
process terminates it at once (TerminateProcess), without that stop.
"""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

CTRL_C_EVENT = 0
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JobObjectExtendedLimitInformation = 9

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in (
        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wt.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wt.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wt.DWORD),
                ("SchedulingClass", wt.DWORD)]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
                ("IoInfo", IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t)]


def kill_on_close_job(process_handle):
    kernel32.CreateJobObjectW.restype = wt.HANDLE
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        return None
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not kernel32.SetInformationJobObject(wt.HANDLE(job), JobObjectExtendedLimitInformation,
                                            ctypes.byref(info), ctypes.sizeof(info)):
        return None
    if not kernel32.AssignProcessToJobObject(wt.HANDLE(job), wt.HANDLE(int(process_handle))):
        return None
    return job


def run(pidfile, cmd):
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0  # SW_HIDE
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=sys.stdout.fileno(), stderr=sys.stderr.fileno(),
                            creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=si)
    job = kill_on_close_job(proc._handle)
    if job is None:
        print(f"winproc: no job object for pid {proc.pid} (error {ctypes.get_last_error()}): "
              "a hard kill of the launcher leaves it running", file=sys.stderr, flush=True)
    with open(pidfile, "w") as f:
        f.write(f"{proc.pid}\n")
    return proc.wait()


def stop(pid):
    # this process leaves its own console (if any) and joins the node's, where Ctrl-C reaches every
    # attached process: the node, and this one, which ignores it
    kernel32.FreeConsole()
    if not kernel32.AttachConsole(wt.DWORD(pid)):
        print(f"winproc: cannot attach to the console of pid {pid} (error {ctypes.get_last_error()})", file=sys.stderr)
        return 1
    kernel32.SetConsoleCtrlHandler(None, True)
    ok = kernel32.GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)
    err = ctypes.get_last_error()
    kernel32.FreeConsole()
    if not ok:
        print(f"winproc: Ctrl-C to the console of pid {pid} failed (error {err})", file=sys.stderr)
        return 1
    return 0


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "run":
        return run(sys.argv[2], sys.argv[3:])
    if len(sys.argv) == 3 and sys.argv[1] == "stop":
        return stop(int(sys.argv[2]))
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
