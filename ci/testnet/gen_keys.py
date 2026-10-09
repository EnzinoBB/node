#!/usr/bin/env python3
"""Generate node key files for a private test network.

Usage: gen_keys.py <count> <work_dir>
Writes <work_dir>/n<i>/NodePublic.txt and NodePrivate.txt for i = 1..count, in the format the node
reads: Base58 of the 32-byte ed25519 public key, and Base58 of the 64-byte libsodium secret key
(seed || public key). The node would otherwise ask for keys interactively.
"""
import os
import sys

import base58
import nacl.signing


def main():
    count, work = int(sys.argv[1]), sys.argv[2]
    for i in range(1, count + 1):
        node_dir = os.path.join(work, f"n{i}")
        os.makedirs(node_dir, exist_ok=True)
        key = nacl.signing.SigningKey.generate()
        public = bytes(key.verify_key)
        secret = bytes(key) + public
        with open(os.path.join(node_dir, "NodePublic.txt"), "w") as f:
            f.write(base58.b58encode(public).decode())
        with open(os.path.join(node_dir, "NodePrivate.txt"), "w") as f:
            f.write(base58.b58encode(secret).decode())
        print(base58.b58encode(public).decode())


if __name__ == "__main__":
    main()
