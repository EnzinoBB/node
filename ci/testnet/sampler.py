#!/usr/bin/env python3
"""Sample the resources a private test network uses, to size the machine that runs it.

Usage: sampler.py <work_dir> <interval_s> <out.tsv> --nodes N --summary-file PATH

Every interval it writes one host row and one row per process of the network to out.tsv:
the nodes (cwd <work>/n<i>, named n<i>), their contract executors (cs.testnet.node=n<i>, named
x<i>) and the harness scripts (py:<script>). Host: memory used and available, swap, load, CPU
busy and steal, free disk and the size of <work>. Process: RSS, CPU percent of one core over the
interval, threads. t and the lowest and highest sequence come from <work>/progress, which run.sh
rewrites every loop.

It runs until SIGTERM, takes a last sample, then writes a summary (numbers only, no verdict) to
--summary-file and stdout. Standard library only, and it never fails the run: errors are logged and
the sampling goes on.
"""
import argparse
import os
import re
import signal
import statistics
import subprocess
import sys
import time

CLK = os.sysconf("SC_CLK_TCK")
HARNESS = ("fund.py", "contracts.py", "garbage.py", "api_load.py")
COLUMNS = ["ts", "t", "seq_min", "seq_max", "kind", "name", "pid", "rss_mb", "cpu_pct", "threads",
           "mem_used_mb", "mem_avail_mb", "swap_mb", "load1", "cpu_busy_pct", "cpu_steal_pct",
           "disk_work_mb", "disk_avail_mb"]

stop = False


def on_term(*_):
    global stop
    stop = True


def log(msg):
    print(f"sampler: {msg}", file=sys.stderr, flush=True)


def read(path):
    with open(path, "rb") as f:
        return f.read().decode(errors="replace")


def meminfo():
    m = {}
    for line in read("/proc/meminfo").splitlines():
        k, v = line.split(":", 1)
        m[k] = int(v.split()[0]) // 1024     # kB -> MB
    return m


def cpu_times():
    # user nice system idle iowait irq softirq steal
    f = [int(x) for x in read("/proc/stat").splitlines()[0].split()[1:9]]
    return sum(f), f[3] + f[4], f[7]


def classify(pid, work):
    try:
        cmd = read(f"/proc/{pid}/cmdline").split("\0")
    except OSError:
        return None
    joined = " ".join(cmd)
    m = re.search(r"cs\.testnet\.node=n(\d+)", joined)
    # the JVM itself (executor.sh execs it), not a shell or pkill naming the marker
    if m and cmd and os.path.basename(cmd[0]) == "java":
        return "exec", f"x{m.group(1)}"
    if "--config-file" in cmd:
        try:
            cwd = os.readlink(f"/proc/{pid}/cwd")
        except OSError:
            return None
        m = re.fullmatch(re.escape(work) + r"/n(\d+)", cwd)
        if m:
            return "node", f"n{m.group(1)}"
        return None
    if cmd and "python" in os.path.basename(cmd[0]):
        for script in HARNESS:
            if any(a.endswith(script) for a in cmd[1:3]):
                return "py", f"py:{script}"
    return None


def proc_stat(pid):
    st = read(f"/proc/{pid}/stat")
    fields = st[st.rindex(")") + 2:].split()
    ticks = int(fields[11]) + int(fields[12])      # utime, stime (fields 14 and 15)
    rss = threads = 0
    for line in read(f"/proc/{pid}/status").splitlines():
        if line.startswith("VmRSS:"):
            rss = int(line.split()[1]) // 1024
        elif line.startswith("Threads:"):
            threads = int(line.split()[1])
    return ticks, rss, threads


def progress(work):
    try:
        kv = dict(p.split("=", 1) for p in read(os.path.join(work, "progress")).split())
        return kv.get("t", ""), kv.get("min", ""), kv.get("max", "")
    except (OSError, ValueError):
        return "", "", ""


def du_mb(path):
    try:
        out = subprocess.run(["du", "-sm", path], capture_output=True, text=True, timeout=30).stdout
        return int(out.split()[0])
    except Exception:
        return ""


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def summary(rows, nodes, interval):
    host = [r for r in rows if r["kind"] == "host"]
    if not host:
        return "resources: no sample taken\n"
    out = []
    dur = (host[-1]["ts"] - host[0]["ts"]) if len(host) > 1 else 0
    out.append(f"resources: {len(host)} samples every {interval} s over {dur / 60:.1f} min, {os.cpu_count()} CPUs, "
               f"{host[0]['mem_used_mb'] + host[0]['mem_avail_mb']} MB RAM")
    peak = max(host, key=lambda r: r["mem_used_mb"])
    out.append(f"  host memory: peak used {peak['mem_used_mb']} MB (t={peak['t']} seq={peak['seq_max']}), "
               f"min available {min(r['mem_avail_mb'] for r in host)} MB, peak swap {max(r['swap_mb'] for r in host)} MB")
    busy = [r["cpu_busy_pct"] for r in host if r["cpu_busy_pct"] != ""]
    if busy:
        hot = sum(1 for b in busy if b >= 90)
        out.append(f"  host CPU: busy mean {statistics.mean(busy):.0f} %, peak {max(busy):.0f} %, "
                   f"{100 * hot / len(busy):.0f} % of samples at 90 % or more, "
                   f"peak steal {max(r['cpu_steal_pct'] for r in host if r['cpu_steal_pct'] != ''):.0f} %, "
                   f"peak load1 {max(r['load1'] for r in host):.1f}")
    disk = [r["disk_work_mb"] for r in host if r["disk_work_mb"] != ""]
    if disk:
        out.append(f"  disk: work dir up to {max(disk)} MB, min free {min(r['disk_avail_mb'] for r in host)} MB")

    procs = {}
    for r in rows:
        if r["kind"] != "host":
            procs.setdefault((r["kind"], r["name"]), []).append(r)

    def key(k):
        m = re.search(r"\d+$", k[1])
        return (k[0], int(m.group()) if m else 0, k[1])

    for kind, title in (("node", "nodes"), ("exec", "executors"), ("py", "harness")):
        keys = sorted((k for k in procs if k[0] == kind), key=key)
        if not keys:
            continue
        out.append(f"  {title}: name, peak RSS MB, mean / peak CPU % of one core, processes (restarts + 1)")
        for k in keys:
            rs = procs[k]
            cpus = [r["cpu_pct"] for r in rs if r["cpu_pct"] != ""]
            out.append(f"    {k[1]:<16} {max(r['rss_mb'] for r in rs):>6} {statistics.mean(cpus) if cpus else 0:>6.0f} "
                       f"{max(cpus) if cpus else 0:>6.0f} {len({r['pid'] for r in rs}):>4}")
        out.append(f"    sum of peak RSS: {sum(max(r['rss_mb'] for r in procs[k]) for k in keys)} MB")
        if kind == "exec":
            # a node that stops stops every executor of the machine (jps), and each node relaunches its own
            out.append(f"    executor relaunches: {sum(len({r['pid'] for r in procs[k]}) - 1 for k in keys)}")
    per_ts = {}
    for r in rows:
        if r["kind"] in ("node", "exec"):
            per_ts[r["ts"]] = per_ts.get(r["ts"], 0) + r["rss_mb"]
    if per_ts:
        out.append(f"  peak RSS of all nodes and executors together: {max(per_ts.values())} MB")

    prog = [(num(r["t"]), num(r["seq_min"]), num(r["seq_max"])) for r in host]
    # from the first block on: before it the nodes start (and wait for their executors)
    prog = [p for p in prog if None not in p and p[2] > 0]
    if len(prog) > 1:
        overall = (prog[-1][2] - prog[0][2]) / max(prog[-1][0] - prog[0][0], 1)
        rates = [(b[2] - a[2]) / (b[0] - a[0]) for a, b in zip(prog, prog[1:]) if b[0] > a[0]]
        spreads = [p[2] - p[1] for p in prog]
        per_interval = (f"per interval min {min(rates):.2f} median {statistics.median(rates):.2f}" if rates
                        else "no interval with progress")
        out.append(f"  blocks: {overall:.2f} per second overall, {per_interval}; spread between nodes up to "
                   f"{max(spreads):.0f}, {sum(1 for s in spreads if s > 50)} samples above 50")
    seen = {}
    for r in rows:
        if r["kind"] == "node":
            seen.setdefault(r["ts"], set()).add(r["name"])
    down = sum(1 for r in host if len(seen.get(r["ts"], ())) < nodes)
    out.append(f"  samples with fewer than {nodes} node processes: {down}")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("interval", type=int)
    ap.add_argument("out")
    ap.add_argument("--nodes", type=int, required=True)
    ap.add_argument("--summary-file", required=True)
    a = ap.parse_args()
    work = os.path.realpath(a.work)
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)

    rows = []
    last_ticks = {}
    last_cpu = cpu_times()
    last_ts = time.time()
    with open(a.out, "w") as out:
        out.write("\t".join(COLUMNS) + "\n")
        while True:
            # sleep in steps, so that SIGTERM takes the last sample at once
            end = time.time() + a.interval
            while not stop and time.time() < end:
                time.sleep(min(1, max(0, end - time.time())))
            try:
                now = time.time()
                elapsed = max(now - last_ts, 1e-3)
                t, smin, smax = progress(work)
                m = meminfo()
                total, idle, steal = cpu_times()
                dt = max(total - last_cpu[0], 1)
                busy = round(100 * (1 - (idle - last_cpu[1]) / dt), 1)
                stl = round(100 * (steal - last_cpu[2]) / dt, 1)
                last_cpu = (total, idle, steal)
                vfs = os.statvfs(work)
                host = {"ts": round(now), "t": t, "seq_min": smin, "seq_max": smax, "kind": "host", "name": "host",
                        "pid": "", "rss_mb": "", "cpu_pct": "", "threads": "",
                        "mem_used_mb": m["MemTotal"] - m["MemAvailable"], "mem_avail_mb": m["MemAvailable"],
                        "swap_mb": m["SwapTotal"] - m["SwapFree"], "load1": float(read("/proc/loadavg").split()[0]),
                        "cpu_busy_pct": busy, "cpu_steal_pct": stl, "disk_work_mb": du_mb(work),
                        "disk_avail_mb": vfs.f_bavail * vfs.f_frsize // (1 << 20)}
                sample = [host]
                ticks_now = {}
                for d in os.listdir("/proc"):
                    if not d.isdigit():
                        continue
                    c = classify(d, work)
                    if not c:
                        continue
                    try:
                        ticks, rss, threads = proc_stat(d)
                    except (OSError, ValueError, IndexError):
                        continue
                    ticks_now[d] = ticks
                    cpu = round(100 * (ticks - last_ticks[d]) / CLK / elapsed, 1) if d in last_ticks else ""
                    sample.append({**{k: "" for k in COLUMNS}, "ts": host["ts"], "t": t, "seq_min": smin,
                                   "seq_max": smax, "kind": c[0], "name": c[1], "pid": int(d), "rss_mb": rss,
                                   "cpu_pct": cpu, "threads": threads})
                last_ticks = ticks_now
                last_ts = now
                for r in sample:
                    out.write("\t".join(str(r[k]) for k in COLUMNS) + "\n")
                out.flush()
                rows.extend(sample)
            except Exception as e:     # never let a sample fail the run
                log(f"sample failed: {e!r}")
            if stop:
                break
    text = summary(rows, a.nodes, a.interval)
    try:
        with open(a.summary_file, "w") as f:
            f.write(text)
    except OSError as e:
        log(f"cannot write {a.summary_file}: {e}")
    sys.stdout.write(text)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log(f"stopped: {e!r}")
