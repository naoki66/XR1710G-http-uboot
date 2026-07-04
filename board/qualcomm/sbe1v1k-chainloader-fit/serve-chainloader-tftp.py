#!/usr/bin/env python3
"""Small read-only TFTP server for SBE1V1K chainloader testing.

This is intentionally minimal: it serves one file, supports RRQ only, and
handles the common U-Boot options needed for tftpboot on a non-root port.
"""

import argparse
import hashlib
import socket
import struct
import sys
from pathlib import Path


OP_RRQ = 1
OP_DATA = 3
OP_ACK = 4
OP_ERROR = 5
OP_OACK = 6


def default_fit_path():
    repo_root = Path(__file__).resolve().parents[3]
    return repo_root / "sbe1v1k-chainloader" / "sbe1v1k-chainloader.itb"


def parse_rrq(packet):
    if len(packet) < 4:
        raise ValueError("short packet")

    op = struct.unpack("!H", packet[:2])[0]
    if op != OP_RRQ:
        raise ValueError(f"unsupported opcode {op}")

    fields = packet[2:].split(b"\0")
    if fields and fields[-1] == b"":
        fields = fields[:-1]
    if len(fields) < 2:
        raise ValueError("malformed RRQ")

    filename = fields[0].decode("utf-8", "replace")
    mode = fields[1].decode("ascii", "ignore").lower()
    options = {}

    for i in range(2, len(fields) - 1, 2):
        key = fields[i].decode("ascii", "ignore").lower()
        val = fields[i + 1].decode("ascii", "ignore")
        if key:
            options[key] = val

    return filename, mode, options


def build_oack(options, file_size):
    accepted = []
    block_size = 512

    if "timeout" in options:
        try:
            timeout = max(1, min(int(options["timeout"]), 255))
        except ValueError:
            timeout = 5
        accepted.extend([b"timeout", str(timeout).encode("ascii")])

    if "tsize" in options:
        accepted.extend([b"tsize", str(file_size).encode("ascii")])

    if "blksize" in options:
        try:
            requested = int(options["blksize"])
        except ValueError:
            requested = 512
        block_size = max(8, min(requested, 1468))
        accepted.extend([b"blksize", str(block_size).encode("ascii")])

    if "windowsize" in options:
        accepted.extend([b"windowsize", b"1"])

    if not accepted:
        return None, block_size

    return struct.pack("!H", OP_OACK) + b"\0".join(accepted) + b"\0", block_size


def send_error(sock, addr, code, message):
    payload = struct.pack("!HH", OP_ERROR, code) + message.encode("ascii") + b"\0"
    sock.sendto(payload, addr)


def serve_once(sock, payload, served_name, timeout, verbose):
    packet, addr = sock.recvfrom(2048)
    filename, mode, options = parse_rrq(packet)

    if verbose:
        print(f"RRQ from {addr[0]}:{addr[1]} file={filename!r} mode={mode} options={options}")

    if mode != "octet":
        send_error(sock, addr, 0, "only octet mode is supported")
        return False

    if Path(filename).name != served_name:
        send_error(sock, addr, 1, "file not found")
        return False

    oack, block_size = build_oack(options, len(payload))
    sock.settimeout(timeout)

    if oack:
        sock.sendto(oack, addr)
        while True:
            try:
                ack, ack_addr = sock.recvfrom(2048)
            except socket.timeout:
                sock.sendto(oack, addr)
                continue
            if ack_addr == addr and len(ack) >= 4 and struct.unpack("!HH", ack[:4]) == (OP_ACK, 0):
                break
    else:
        block_size = 512

    block = 1
    offset = 0

    while True:
        chunk = payload[offset:offset + block_size]
        data = struct.pack("!HH", OP_DATA, block & 0xffff) + chunk

        while True:
            sock.sendto(data, addr)
            try:
                ack, ack_addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            if ack_addr != addr or len(ack) < 4:
                continue
            op, ack_block = struct.unpack("!HH", ack[:4])
            if op == OP_ACK and ack_block == (block & 0xffff):
                break

        offset += len(chunk)
        if len(chunk) < block_size:
            if verbose:
                print(f"sent {offset} bytes to {addr[0]}:{addr[1]}")
            return True

        block += 1


def main():
    parser = argparse.ArgumentParser(description="Serve one file over read-only TFTP.")
    parser.add_argument("--file", default=str(default_fit_path()))
    parser.add_argument("--name", default="sbe1v1k-chainloader.itb")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=6969)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--once", action="store_true", help="exit after one transfer attempt")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    path = Path(args.file)
    payload = path.read_bytes()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))

    print(f"Serving {path} as {args.name} on {args.host}:{args.port}")
    print(f"Size {len(payload)} bytes, SHA256 {hashlib.sha256(payload).hexdigest()}")
    sys.stdout.flush()

    while True:
        try:
            serve_once(sock, payload, args.name, args.timeout, args.verbose)
        except KeyboardInterrupt:
            return 0
        except Exception as exc:
            print(f"transfer failed: {exc}", file=sys.stderr)
            if args.once:
                return 1
        if args.once:
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
