#!/usr/bin/env python3
"""Debug probe of the node's public API (SyncStateGet) with timings and server-side evidence.

  api_probe.py --api 127.0.0.1:9090 --pid PID --minutes N --out DIR [--interval S] [--mode reuse|new]

mode reuse: one client kept until a call fails, as ci/snapshot/check_node.py wait-head does;
mode new:   a new connection for every call.
On a failure: lsof of the node, the sockets on the port and an immediate second call on a fresh
connection (timed). Exit 0 when every fresh-connection call got an answer, 1 otherwise.
"""
import argparse
import datetime
import os
import subprocess
import sys
import threading
import time

import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

HERE = os.path.dirname(os.path.abspath(__file__))
IDL = os.path.abspath(os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
MOD = thriftpy2.load(os.path.join(IDL, "api.thrift"), module_name="api_thrift", include_dirs=[IDL])


def now():
    return datetime.datetime.utcnow().strftime("%H:%M:%S.%f")[:-3]


def client(api, timeout_ms):
    host, port = api.rsplit(":", 1)
    return make_client(MOD.API, host=host, port=int(port), proto_factory=TBinaryProtocolFactory(),
                       trans_factory=TBufferedTransportFactory(), timeout=timeout_ms)


def run(cmd, path):
    try:
        out = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=60).stdout
    except Exception as e:  # noqa: BLE001 - evidence only
        out = f"{cmd}: {e}\n"
    with open(path, "a") as f:
        f.write(f"==== {now()} {cmd}\n{out}\n")
    return out


HANG = {"pid": 0, "out": ".", "n": 0}


def call(node):
    """One SyncStateGet; a call still waiting after 5 s has the node's fds and threads recorded."""
    t0 = time.time()
    done = threading.Event()

    def watch():
        if not done.wait(5) and HANG["n"] < 6:
            HANG["n"] += 1
            tag = f"{HANG['n']:02d}"
            run(f"lsof -n -p {HANG['pid']}", os.path.join(HANG["out"], f"hang-lsof-{tag}.txt"))
            run(f"netstat -an -p tcp | grep -E '[.:]9090 '", os.path.join(HANG["out"], f"hang-netstat-{tag}.txt"))
            run(f"sample {HANG['pid']} 2", os.path.join(HANG["out"], f"hang-sample-{tag}.txt"))

    threading.Thread(target=watch, daemon=True).start()
    try:
        s = node.SyncStateGet()
        return True, time.time() - t0, f"block {s.lastBlock} round {s.currRound} gap {s.currRound - s.lastBlock}"
    except Exception as e:  # noqa: BLE001
        return False, time.time() - t0, repr(e)
    finally:
        done.set()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default="127.0.0.1:9090")
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--minutes", type=float, default=20)
    ap.add_argument("--interval", type=float, default=30)
    ap.add_argument("--mode", choices=("reuse", "new"), default="reuse")
    ap.add_argument("--timeout-ms", type=int, default=120000)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    HANG["pid"], HANG["out"] = args.pid, args.out
    port = args.api.rsplit(":", 1)[1]
    log = open(os.path.join(args.out, f"probe-{args.mode}.txt"), "a", buffering=1)

    def say(text):
        line = f"{now()} {text}"
        print(line, flush=True)
        log.write(line + "\n")

    say(f"probe {args.mode} every {args.interval}s for {args.minutes} min, node pid {args.pid}")
    run("ulimit -n; ulimit -u; sysctl kern.maxfiles kern.maxfilesperproc kern.num_taskthreads 2>/dev/null",
        os.path.join(args.out, "limits.txt"))
    deadline = time.time() + args.minutes * 60
    node, fresh_fail, fresh_ok, failures, n = None, 0, 0, 0, 0
    while time.time() < deadline:
        n += 1
        try:
            os.kill(args.pid, 0)
        except OSError:
            say("the node is gone")
            return 1
        fresh = node is None or args.mode == "new"
        try:
            if fresh:
                node = client(args.api, args.timeout_ms)
            ok, dt, text = call(node)
        except Exception as e:  # noqa: BLE001 - connect failure
            ok, dt, text = False, 0.0, repr(e)
        fds = run(f"lsof -n -p {args.pid} 2>/dev/null | wc -l", os.path.join(args.out, "fdcount.txt")).strip()
        say(f"#{n} {'fresh' if fresh else 'reused'} {'OK' if ok else 'FAIL'} {dt:.3f}s {text} fds={fds}")
        if fresh:
            fresh_ok += ok
            fresh_fail += not ok
        if not ok:
            failures += 1
            node = None
            if failures <= 4 or failures % 5 == 0:
                tag = f"{failures:03d}"
                run(f"lsof -n -p {args.pid}", os.path.join(args.out, f"lsof-{tag}.txt"))
                run(f"netstat -an -p tcp | grep -E '[.:]{port} '", os.path.join(args.out, f"netstat-{tag}.txt"))
                # a fresh connection at once: does it fail too, and how fast
                try:
                    ok2, dt2, text2 = call(client(args.api, args.timeout_ms))
                except Exception as e:  # noqa: BLE001
                    ok2, dt2, text2 = False, 0.0, repr(e)
                say(f"#{n} immediate fresh {'OK' if ok2 else 'FAIL'} {dt2:.3f}s {text2}")
                fresh_ok += ok2
                fresh_fail += not ok2
                run(f"netstat -an -p tcp | grep -E '[.:]{port} '", os.path.join(args.out, f"netstat-{tag}.txt"))
        time.sleep(args.interval)
    say(f"done: fresh connections {fresh_ok} answered, {fresh_fail} failed; {failures} failed calls in all")
    return 0 if fresh_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
