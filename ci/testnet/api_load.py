#!/usr/bin/env python3
"""Loads blocks through a node's public API until stopped.

Usage: api_load.py HOST:PORT STATS_FILE [--threads N]

Each thread asks PoolInfoGet for random sequences among the last 1,500 blocks (on a pruned node most
of them are pruned or about to be), so that the API threads read blocks while the node thread prunes.
Every few seconds STATS_FILE gets "calls=<n> errors=<n>"; a node that restarts only counts errors
until it answers again. Stops on SIGTERM.
"""
import argparse
import os
import random
import signal
import sys
import threading
import time

import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

HERE = os.path.dirname(os.path.abspath(__file__))
IDL = os.path.abspath(os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
api = thriftpy2.load(os.path.join(IDL, "api.thrift"), module_name="api_thrift", include_dirs=[IDL])

stop = threading.Event()
lock = threading.Lock()
calls = 0
errors = 0


def worker(host, port):
    global calls, errors
    node = None
    while not stop.is_set():
        try:
            if node is None:
                node = make_client(api.API, host=host, port=port, proto_factory=TBinaryProtocolFactory(),
                                   trans_factory=TBufferedTransportFactory(), timeout=10000)
            head = node.SyncStateGet().lastBlock
            for _ in range(50):
                seq = random.randint(max(0, head - 1500), max(0, head))
                node.PoolInfoGet(seq, 0)
                with lock:
                    calls += 1
        except Exception:
            with lock:
                errors += 1
            node = None
            time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("api")
    parser.add_argument("stats")
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    host, port = args.api.rsplit(":", 1)
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    threads = [threading.Thread(target=worker, args=(host, int(port)), daemon=True) for _ in range(args.threads)]
    for t in threads:
        t.start()
    while not stop.is_set():
        with lock:
            line = f"calls={calls} errors={errors}\n"
        with open(args.stats, "w") as f:
            f.write(line)
        stop.wait(5)
    with lock:
        line = f"calls={calls} errors={errors}\n"
    with open(args.stats, "w") as f:
        f.write(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
