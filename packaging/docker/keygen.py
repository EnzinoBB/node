#!/usr/bin/env python3
"""Generate the node's key pair: keygen.py <keys dir>.

Writes NodePublic.txt (Base58 of the 32-byte ed25519 public key) and NodePrivate.txt (Base58 of the
64-byte libsodium secret key, seed then public key), the format the node reads, readable by the
owner only. Refuses to overwrite existing keys.
"""
import os
import sys

import base58
import nacl.signing


def main():
    keys = sys.argv[1]
    public_path = os.path.join(keys, "NodePublic.txt")
    private_path = os.path.join(keys, "NodePrivate.txt")
    if os.path.exists(public_path) or os.path.exists(private_path):
        sys.exit(f"keys already exist in {keys}")
    os.makedirs(keys, mode=0o700, exist_ok=True)
    key = nacl.signing.SigningKey.generate()
    public = bytes(key.verify_key)
    old = os.umask(0o077)
    try:
        with open(private_path, "w") as f:
            f.write(base58.b58encode(bytes(key) + public).decode())
        with open(public_path, "w") as f:
            f.write(base58.b58encode(public).decode())
    finally:
        os.umask(old)
    print(base58.b58encode(public).decode())


if __name__ == "__main__":
    main()
