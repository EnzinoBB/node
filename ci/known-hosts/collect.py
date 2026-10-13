#!/usr/bin/env python3
"""Build a verified known_hosts list from a short run of a node on mainnet.

Usage: collect.py <node dir> <out dir> [--min-replies 0.5]

<node dir> is the csnode directory of a node that ran for a while from an empty database: its
log/log_*.txt (info level is enough) and the known_hosts.txt it rewrote. A node syncing from
block 0 asks its neighbours for the oldest blocks first, so a peer that answers those requests
keeps the chain's history; one that ignores them does not, or is unreachable for blocks.

Writes to <out dir>:
- known_hosts.txt: "ip:port key" for the peers that connected, report a height close to the median
  of the others and answered at least --min-replies of the block requests sent to them, at most
  --max-per-subnet per /24 (/48 for IPv6), best first. This limits, but cannot prevent, a party with
  many peers on many networks from filling the list, so the list is reviewed before it is shipped;
- peers.json and peers.md: every peer seen, with its address, last sequence, requests, replies
  and the range of blocks asked from it.
"""
import argparse
import glob
import ipaddress
import json
import os
import re

import base58

NEIGHBOUR = re.compile(r"NODE: new neighbour added (\w+) last seq (\d+) last round (\d+)")
REMOVED = re.compile(r"NODE: neighbour removed (\w+)")
REQUEST = re.compile(r"SYNC: requesting for (\d+) blocks \[(\d+), (\d+)\] from ([0-9A-F]{64})")
REPLY = re.compile(r"getBlockReply from ([0-9A-F]{64})")
LAST_WRITTEN = re.compile(r"Last written sequence = ([\d']+)")


def hex_of(key58):
    return base58.b58decode(key58).hex().upper()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("node_dir")
    parser.add_argument("out_dir")
    parser.add_argument("--min-replies", type=float, default=0.5)
    parser.add_argument("--max-per-subnet", type=int, default=3,
                        help="peers listed per /24, so a single operator cannot fill the list")
    parser.add_argument("--max-lag", type=int, default=1000,
                        help="blocks a peer may be away from the median height reported by the others")
    args = parser.parse_args()

    addresses = {}  # hex key -> "ip:port", from the node's own known_hosts.txt and the shipped one
    for path in (os.path.join(args.node_dir, "known_hosts.txt"),):
        if os.path.exists(path):
            for line in open(path):
                parts = line.split()
                if len(parts) == 2:
                    try:
                        addresses[hex_of(parts[1])] = parts[0]
                    except ValueError:
                        pass

    peers = {}

    def peer(hex_key):
        return peers.setdefault(hex_key, {"key": base58.b58encode(bytes.fromhex(hex_key)).decode(),
                                          "address": addresses.get(hex_key, ""), "connected": False,
                                          "last_seq": 0, "removed": 0, "requests": 0, "blocks_requested": 0,
                                          "replies": 0, "first_block": None, "last_block": None})

    synced = 0
    logs = sorted(glob.glob(os.path.join(args.node_dir, "log", "log_*.txt")), key=os.path.getmtime)
    for path in logs:
        for line in open(path, errors="replace"):
            m = NEIGHBOUR.search(line)
            if m:
                p = peer(hex_of(m.group(1)))
                p["connected"] = True
                p["last_seq"] = max(p["last_seq"], int(m.group(2)))
                continue
            m = REQUEST.search(line)
            if m:
                p = peer(m.group(4))
                p["requests"] += 1
                p["blocks_requested"] += int(m.group(1))
                first, last = int(m.group(2)), int(m.group(3))
                p["first_block"] = first if p["first_block"] is None else min(p["first_block"], first)
                p["last_block"] = last if p["last_block"] is None else max(p["last_block"], last)
                continue
            m = REPLY.search(line)
            if m:
                peer(m.group(1))["replies"] += 1
                continue
            m = REMOVED.search(line)
            if m:
                peer(hex_of(m.group(1)))["removed"] += 1
                continue
            m = LAST_WRITTEN.search(line)
            if m:
                synced = max(synced, int(m.group(1).replace("'", "")))

    # The list is advice for new nodes, so it must not be easy to fill with one party's peers (eclipse):
    # only peers on the tip of the chain everybody else reports, and a few per /24 at most. It still
    # reaches the packages only through a reviewed pull request.
    # The tip is the median of the heights peers report, one value per /24 so that many peers of one
    # party cannot move it; a peer claiming far more than that is as suspect as one far behind.
    def subnet_of(p):
        """The /24 (IPv4) or /48 (IPv6) of the peer's address, None when it is not a plain IP:port."""
        host, sep, port = p["address"].rpartition(":")
        if not sep or not port.isdigit():
            return None
        try:
            ip = ipaddress.ip_address(host.strip("[]"))
        except ValueError:
            return None
        return str(ipaddress.ip_network(f"{ip}/{24 if ip.version == 4 else 48}", strict=False))

    by_subnet = {}
    for p in peers.values():
        if p["connected"] and subnet_of(p):
            by_subnet.setdefault(subnet_of(p), []).append(p["last_seq"])
    heights = sorted(sorted(seqs)[len(seqs) // 2] for seqs in by_subnet.values())
    tip = heights[len(heights) // 2] if heights else 0
    for p in peers.values():
        p["reply_ratio"] = round(p["replies"] / p["requests"], 3) if p["requests"] else None
        p["serves_history"] = p["requests"] > 0 and p["reply_ratio"] >= args.min_replies
        p["on_tip"] = abs(p["last_seq"] - tip) <= args.max_lag
        # only peers that actually served blocks: one that connected too late to be asked proved nothing
        p["listed"] = subnet_of(p) is not None and p["connected"] and p["on_tip"] and p["serves_history"]
        p["note"] = ""

    ordered = sorted(peers.values(), key=lambda p: (not p["listed"], -(p["reply_ratio"] or 0), -p["last_seq"]))
    per_subnet = {}
    for p in ordered:
        if p["listed"]:
            subnet = subnet_of(p)
            per_subnet[subnet] = per_subnet.get(subnet, 0) + 1
            if per_subnet[subnet] > args.max_per_subnet:
                p["listed"] = False
                p["note"] = f"more than {args.max_per_subnet} peers in {subnet}"
        elif p["connected"] and not p["on_tip"]:
            p["note"] = f"more than {args.max_lag} blocks from the median height {tip}"
    ordered.sort(key=lambda p: (not p["listed"], -(p["reply_ratio"] or 0), -p["last_seq"]))
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "known_hosts.txt"), "w") as f:
        for p in ordered:
            if p["listed"]:
                f.write(f"{p['address']} {p['key']}\n")
    with open(os.path.join(args.out_dir, "peers.json"), "w") as f:
        json.dump({"synced_to": synced, "peers": ordered}, f, indent=1)
    with open(os.path.join(args.out_dir, "peers.md"), "w") as f:
        listed = sum(p["listed"] for p in ordered)
        f.write(f"{listed} peers listed out of {len(ordered)} seen; the node synced blocks 0 to {synced}.\n\n")
        f.write("| Key | Address | Listed | Last seq | Requests | Replies | Blocks asked | Note |\n")
        f.write("| --- | --- | --- | --- | --- | --- | --- | --- |\n")
        for p in ordered:
            asked = f"{p['first_block']}–{p['last_block']}" if p["requests"] else ""
            f.write(f"| {p['key']} | {p['address'] or 'unknown'} | {'yes' if p['listed'] else 'no'} | {p['last_seq']} "
                    f"| {p['requests']} | {p['replies']} | {asked} | {p['note']} |\n")
    print(f"{sum(p['listed'] for p in ordered)} peers listed of {len(ordered)} seen, synced to {synced}")


if __name__ == "__main__":
    main()
