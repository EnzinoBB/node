#!/usr/bin/env python3
"""The snapshot sources of the weekly known-hosts job (docs/specs/2026-10-25-snapshot-p2p-design.md, 10).

The job runs `node --survey-snapshots 600` after collect.py in the same node folder. The node prints
one line per snapshot the neighbours declared and one per serving peer that gave no manifest:

    SURVEY group seq=<n> head=<hex> state=<hex> settings=<hex> sources=<key>@<address>@<created>,...
    SURVEY silent <key>@<address>
    SURVEY end groups=<n> serving=<n> silent=<n>

From that output and the node's known_peers.json this script writes

  snapshot_sources.txt  "ip:port key" lines, the peers that answered a manifest that verified, at most
                        --max-per-subnet per /24 (/48 for IPv6) so that one party cannot fill the
                        file; the strongest snapshot's sources first. Shipped in the packages: it
                        only says which peers to bring into the neighbour table first, the node
                        finds every serving peer anyway.
  snapshots.md          what was seen: the snapshots with their sources, the serving peers that gave
                        no manifest, and the count of independent serving addresses that the
                        `min_sources` decision depends on.

Usage:  snapshot_sources.py SURVEY_OUTPUT KNOWN_PEERS_JSON OUT_DIR [--max-per-subnet N]
        snapshot_sources.py --self-test
"""
import argparse
import datetime
import ipaddress
import json
import os
import re
import sys
import tempfile

SNAPSHOT_PROTOCOL_V1 = 1   # net/hostdata.hpp: the capability byte of the peers that serve snapshots
# the output comes from a node that talked to untrusted peers: every field is checked against its
# exact shape before it reaches a file or a pull request
HEX = r"[0-9a-fA-F]{64}"
GROUP = re.compile(rf"^SURVEY group seq=(\d{{1,12}}) head=({HEX}) state=({HEX}) settings=({HEX}) sources=(\S*)$")
CREATED = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$")
SILENT = re.compile(r"^SURVEY silent (\S+)$")
END = re.compile(r"^SURVEY end groups=(\d+) serving=(\d+) silent=(\d+)$")
KEY = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,64}$")


def subnet(address):
    """The /24 (IPv4) or /48 (IPv6) of an address, None when it is not an IP."""
    try:
        ip = ipaddress.ip_address(address.strip("[]"))
    except ValueError:
        return None
    return str(ipaddress.ip_network(f"{ip}/{24 if ip.version == 4 else 48}", strict=False))


def parse_survey(text):
    """-> (groups, silent, summary). Lines that are not survey lines (log output) are ignored."""
    groups, silent, summary = [], [], None
    for line in text.splitlines():
        line = line.strip()
        if m := GROUP.match(line):
            sources = []
            for item in filter(None, m.group(5).split(",")):
                parts = item.split("@")
                if len(parts) == 3 and KEY.match(parts[0]) and subnet(parts[1]) and CREATED.match(parts[2]):
                    sources.append({"key": parts[0], "address": parts[1], "created": parts[2]})
            groups.append({"seq": int(m.group(1)), "head": m.group(2), "state": m.group(3),
                           "settings": m.group(4), "sources": sources})
        elif m := SILENT.match(line):
            parts = m.group(1).split("@")
            if len(parts) == 2 and KEY.match(parts[0]) and subnet(parts[1]):
                silent.append({"key": parts[0], "address": parts[1]})
        elif m := END.match(line):
            summary = {"groups": int(m.group(1)), "serving": int(m.group(2)), "silent": int(m.group(3))}
    groups.sort(key=lambda g: (-len(g["sources"]), -g["seq"]))
    return groups, silent, summary


def load_peers(path):
    """known_peers.json -> {key: peer}; a missing or damaged file is no peers."""
    try:
        peers = json.load(open(path))
    except (OSError, ValueError):
        return {}
    return {p["id"]: p for p in peers if isinstance(p, dict) and isinstance(p.get("id"), str)}


def sources_lines(groups, peers, max_per_subnet):
    """The "ip:port key" lines: strongest group first, one line per key, a few per /24."""
    lines, seen, per_subnet = [], set(), {}
    for g in groups:
        for s in g["sources"]:
            peer = peers.get(s["key"])
            net = subnet(s["address"])
            if s["key"] in seen or peer is None or net is None or not isinstance(peer.get("port"), int):
                continue
            if per_subnet.get(net, 0) >= max_per_subnet:
                continue
            host = s["address"]
            lines.append(f"{'[' + host + ']' if ':' in host else host}:{peer['port']} {s['key']}")
            seen.add(s["key"])
            per_subnet[net] = per_subnet.get(net, 0) + 1
    return lines


def report(groups, silent, summary, peers, today):
    serving_peers = [p for p in peers.values() if int(p.get("capabilities", 0)) & SNAPSHOT_PROTOCOL_V1]
    serving_subnets = {subnet(p.get("ip", "")) for p in serving_peers} - {None}
    out = [f"# Snapshots served on mainnet, {today}", "",
           "A node asked its neighbours for snapshot manifests for several minutes (`node --survey-snapshots`).", "",
           f"- peers known to the node: {len(peers)}",
           f"- peers announcing that they serve snapshots: {len(serving_peers)}, in {len(serving_subnets)} different /24 (/48) networks",
           f"- snapshots declared: {len(groups)}; serving peers that gave no manifest: {len(silent)}"]
    if summary is None:
        out += ["", "_The survey output had no summary line: the node may have stopped early._"]
    out += ["", "`min_sources` (the sources that must agree) counts independent networks: "
                f"**{len(serving_subnets)}** serving network(s) are known now.", ""]
    if groups:
        out += ["| block | state digest | head | created | sources | independent |", "|---|---|---|---|---|---|"]
        for g in groups:
            nets = {subnet(s["address"]) for s in g["sources"]} - {None}
            created = max((s["created"] for s in g["sources"]), default="")
            out.append(f"| {g['seq']} | `{g['state'][:16]}…` | `{g['head'][:16]}…` | {created} | {len(g['sources'])} | {len(nets)} |")
        out.append("")
        for g in groups:
            out += [f"### Block {g['seq']}", "",
                    f"state digest `{g['state']}`, head `{g['head']}`, settings `{g['settings']}`", ""]
            out += [f"- `{s['key']}` at {s['address']} (created {s['created']})" for s in g["sources"]]
            out.append("")
    else:
        out += ["No snapshot was declared.", ""]
    if silent:
        out += ["### Serving peers without a manifest", ""]
        out += [f"- `{s['key']}` at {s['address']}" for s in silent]
        out.append("")
    return "\n".join(out)


def run(survey_path, peers_path, out_dir, max_per_subnet):
    groups, silent, summary = parse_survey(open(survey_path, errors="replace").read())
    peers = load_peers(peers_path)
    os.makedirs(out_dir, exist_ok=True)
    lines = sources_lines(groups, peers, max_per_subnet)
    with open(os.path.join(out_dir, "snapshot_sources.txt"), "w") as f:
        f.write("# serving nodes that answered a snapshot manifest, from the weekly survey\n")
        f.write("".join(line + "\n" for line in lines))
    today = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    with open(os.path.join(out_dir, "snapshots.md"), "w") as f:
        f.write(report(groups, silent, summary, peers, today) + "\n")
    print(f"{len(groups)} snapshots, {len(silent)} silent serving peers, {len(lines)} sources listed")
    return 0


def self_test():
    k1, k2, k3, k4, k5 = ("1" * 40, "2" * 40, "3" * 40, "4" * 40, "5" * 40)
    h = ["aa" * 32, "bb" * 32, "cc" * 32, "dd" * 32, "ee" * 32, "ff" * 32]
    survey = "\n".join([
        "[2026-10-27 10:00:00.1] [info] some log line",
        f"SURVEY group seq=186600000 head={h[0]} state={h[1]} settings={h[2]} sources={k1}@10.0.1.5@2026-10-27T00:00:00Z,"
        f"{k2}@10.0.1.6@2026-10-27T00:00:00Z,{k3}@10.0.1.7@2026-10-27T00:00:00Z,"
        f"{k4}@2001:db8:1::1@2026-10-27T00:00:00Z",
        f"SURVEY group seq=186500000 head={h[3]} state={h[4]} settings={h[5]} sources={k5}@10.0.9.9@2026-10-26T00:00:00Z",
        f"SURVEY silent {k5}@10.0.9.10",
        "SURVEY group seq=bad",
        "SURVEY end groups=2 serving=6 silent=1", ""])
    groups, silent, summary = parse_survey(survey)
    assert [g["seq"] for g in groups] == [186600000, 186500000], groups
    assert len(groups[0]["sources"]) == 4 and silent == [{"key": k5, "address": "10.0.9.10"}], (groups, silent)
    assert summary == {"groups": 2, "serving": 6, "silent": 1}, summary
    peers = {k: {"id": k, "ip": ip, "port": 9100 + i, "capabilities": 1} for i, (k, ip) in enumerate(
        [(k1, "10.0.1.5"), (k2, "10.0.1.6"), (k3, "10.0.1.7"), (k4, "2001:db8:1::1"), (k5, "10.0.9.9")])}
    peers["6" * 40] = {"id": "6" * 40, "ip": "10.0.7.7", "port": 1, "capabilities": 0}
    lines = sources_lines(groups, peers, 2)
    # two per /24: k3 is the third of 10.0.1.0/24; the IPv6 source has its own /48
    assert lines == [f"10.0.1.5:9100 {k1}", f"10.0.1.6:9101 {k2}", f"[2001:db8:1::1]:9103 {k4}", f"10.0.9.9:9104 {k5}"], lines
    assert sources_lines(groups, {}, 2) == [], "no port without known_peers.json"
    # what is not of the exact shape does not parse: keys, addresses, hashes, dates, markup
    g, s, _ = parse_survey(f"SURVEY group seq=1 head={h[0]} state={h[1]} settings={h[2]} sources=nokey@1.2.3.4@2026-10-27T00:00:00Z,"
                           f"{k1}@not-an-ip@2026-10-27T00:00:00Z,{k1}@10.0.0.1@<script>,{k1}@10.0.0.1@2026-10-27T00:00:00Z@x,@@\n"
                           "SURVEY silent bad\nSURVEY silent " + k1 + "@<b>\n"
                           "SURVEY group seq=1 head=a state=b settings=c sources=" + k1 + "@10.0.0.1@2026-10-27T00:00:00Z\n"
                           "SURVEY group seq=99999999999999 head=" + h[0] + " state=" + h[1] + " settings=" + h[2] + " sources=")
    assert len(g) == 1 and g[0]["sources"] == [] and s == [], (g, s)
    with tempfile.TemporaryDirectory() as tmp:
        sp, pp, out = (os.path.join(tmp, n) for n in ("survey.txt", "known_peers.json", "out"))
        open(sp, "w").write(survey)
        json.dump(list(peers.values()), open(pp, "w"))
        assert run(sp, pp, out, 3) == 0
        text = open(os.path.join(out, "snapshots.md")).read()
        assert "186600000" in text and "min_sources" in text and "Serving peers without a manifest" in text, text
        assert open(os.path.join(out, "snapshot_sources.txt")).read().count("\n") == 6   # 1 comment + 5 sources
        # no survey summary, no peers file
        open(sp, "w").write("")
        assert run(sp, os.path.join(tmp, "missing.json"), out, 3) == 0
        assert "No snapshot was declared." in open(os.path.join(out, "snapshots.md")).read()
    print("self-test ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("survey", nargs="?")
    parser.add_argument("known_peers", nargs="?")
    parser.add_argument("out_dir", nargs="?")
    parser.add_argument("--max-per-subnet", type=int, default=3)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not (args.survey and args.known_peers and args.out_dir):
        parser.error("SURVEY_OUTPUT KNOWN_PEERS_JSON OUT_DIR are required")
    return run(args.survey, args.known_peers, args.out_dir, args.max_per_subnet)


if __name__ == "__main__":
    sys.exit(main())
