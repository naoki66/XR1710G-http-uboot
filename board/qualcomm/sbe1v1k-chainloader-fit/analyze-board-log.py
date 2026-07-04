#!/usr/bin/env python3
"""Analyze SBE1V1K U-Boot network debug logs.

The script is intentionally conservative: it only reports evidence visible in
the captured serial log, and keeps the final verdict focused on the next board
debug step.
"""

import argparse
import re
import sys
from pathlib import Path


MISC_BITS = (
    (0x01, "AXI_RD_ERR"),
    (0x02, "AXI_WR_ERR"),
    (0x04, "RX_DESC_FIFO_FULL"),
    (0x08, "RX_ERR_BUF_SIZE"),
    (0x10, "TX_SRAM_FULL"),
    (0x20, "TX_CMPL_BUF_FULL"),
    (0x40, "DATA_LEN_ERR"),
    (0x80, "TX_TIMEOUT"),
)

TX_LINE_RE = re.compile(
    r"EDMA TX len=(?P<len>\d+)"
    r"(?: hw_len=(?P<hw_len>\d+))?"
    r".*?tdes5=0x(?P<tdes5>[0-9a-fA-F]+)"
)
BANNER_RE = re.compile(r"U-Boot (?P<release>\d{4}\.\d{2}[^\s]*)")
MISC_RE = re.compile(r"misc=0x(?P<misc>[0-9a-fA-F]+)")
TXCMPL_RE = re.compile(
    r"txcmpl(?P<ring>\d+).*?errors=0x(?P<errors>[0-9a-fA-F]+)"
    r"(?:\((?P<names>[^)]*)\))?"
)
SUMMARY_TX_RE = re.compile(
    r"txd0=(?P<txd_prod>\d+)/(?:\d+\s+)?(?P<txd_cons>\d+)\s+"
    r"txc0=(?P<txc_prod>\d+)/(?:\d+\s+)?(?P<txc_cons>\d+)"
)
COUNTER_PACKETS_RE = re.compile(
    r"\b(?P<name>port_tx|queue_tx|ppe_port_tx|ppe_queue_tx)\b.*?"
    r"packets=(?P<packets>\d+)"
)
GMAC_TX_RE = re.compile(r"\bgmac_tx_mib:.*?txbytes=0x(?P<txbytes>[0-9a-fA-F]+)")
PPE_TX_PATH_RE = re.compile(
    r"\bppe_tx_path:.*?txbytes=0x(?P<txbytes>[0-9a-fA-F]+)"
)


def repo_release():
    srctree = Path(__file__).resolve().parents[3]
    release = srctree / "include" / "config" / "uboot.release"
    try:
        return release.read_text(encoding="ascii").strip()
    except OSError:
        return None


def misc_names(value):
    names = [name for bit, name in MISC_BITS if value & bit]
    known = sum(bit for bit, _name in MISC_BITS)
    unknown = value & ~known
    if unknown:
        names.append(f"UNKNOWN_0x{unknown:x}")
    return "|".join(names) if names else "none"


def read_log(path):
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8", errors="replace")


def collect(text):
    banners = BANNER_RE.findall(text)
    tx_lines = []
    for match in TX_LINE_RE.finditer(text):
        length = int(match.group("len"))
        hw_len = match.group("hw_len")
        tdes5 = int(match.group("tdes5"), 16)
        tx_lines.append(
            {
                "length": length,
                "hw_len": int(hw_len) if hw_len is not None else None,
                "tdes5": tdes5,
                "line": match.group(0),
            }
        )

    misc_values = [int(match.group("misc"), 16) for match in MISC_RE.finditer(text)]

    txcmpl_errors = []
    for match in TXCMPL_RE.finditer(text):
        errors = int(match.group("errors"), 16)
        txcmpl_errors.append(
            {
                "ring": int(match.group("ring")),
                "errors": errors,
                "names": match.group("names") or "none",
            }
        )

    summaries = []
    for match in SUMMARY_TX_RE.finditer(text):
        summaries.append({key: int(match.group(key)) for key in match.groupdict()})

    counters = {}
    for match in COUNTER_PACKETS_RE.finditer(text):
        name = match.group("name")
        counters.setdefault(name, []).append(int(match.group("packets")))
    for match in GMAC_TX_RE.finditer(text):
        counters.setdefault("gmac_tx_bytes", []).append(int(match.group("txbytes"), 16))
    for match in PPE_TX_PATH_RE.finditer(text):
        counters.setdefault("ppe_tx_path_bytes", []).append(int(match.group("txbytes"), 16))

    return {
        "banners": banners,
        "tx_lines": tx_lines,
        "misc_values": misc_values,
        "txcmpl_errors": txcmpl_errors,
        "summaries": summaries,
        "counters": counters,
        "has_snapshot": "NSS snapshot:" in text,
        "has_ping_fail": "ping failed" in text,
    }


def print_list(title, values):
    print(title)
    if not values:
        print("  <none>")
        return
    for value in values:
        print(f"  {value}")


def analyze(data, expected_release):
    banners = data["banners"]
    latest_banner = banners[-1] if banners else None
    saw_expected = bool(expected_release and expected_release in banners)
    tx_lines = data["tx_lines"]
    short_tx = [tx for tx in tx_lines if tx["length"] < 49]
    padded_tx = [
        tx for tx in short_tx
        if tx["hw_len"] is not None and tx["hw_len"] >= 49 and tx["tdes5"] >= 49
    ]
    unpadded_tx = [
        tx for tx in short_tx
        if tx["hw_len"] is None or tx["hw_len"] < 49 or tx["tdes5"] < 49
    ]
    misc_nonzero = sorted(set(value for value in data["misc_values"] if value))
    txcmpl_nonzero = [entry for entry in data["txcmpl_errors"] if entry["errors"]]
    last_summary = data["summaries"][-1] if data["summaries"] else None

    print("SBE1V1K U-Boot network log analysis")
    print(f"Expected release: {expected_release or '<not set>'}")
    print_list("Seen release banners:", banners)

    if latest_banner:
        if saw_expected:
            print(f"Release verdict: OK, saw expected {expected_release}")
        else:
            print(f"Release verdict: MISMATCH, latest seen {latest_banner}")
    else:
        print("Release verdict: no U-Boot banner found")

    print()
    print(f"EDMA TX lines: {len(tx_lines)} total, {len(short_tx)} short-frame lines")
    if short_tx:
        first = short_tx[0]
        print(
            "First short TX: "
            f"len={first['length']} hw_len={first['hw_len']} "
            f"tdes5=0x{first['tdes5']:08x}"
        )
    print(f"Short-frame padding verdict: padded={len(padded_tx)} unpadded={len(unpadded_tx)}")

    if misc_nonzero:
        print()
        print("EDMA misc status observed:")
        for value in misc_nonzero:
            print(f"  0x{value:08x} ({misc_names(value)})")
    else:
        print()
        print("EDMA misc status observed: none/nonzero not found")

    if txcmpl_nonzero:
        print()
        print("TX completion errors observed:")
        for entry in txcmpl_nonzero:
            print(
                f"  txcmpl{entry['ring']} errors=0x{entry['errors']:08x}"
                f" ({entry['names']})"
            )
    else:
        print()
        print("TX completion errors observed: none/nonzero not found")

    if last_summary:
        print()
        print(
            "Last summary TX rings: "
            f"txd0={last_summary['txd_prod']}/{last_summary['txd_cons']} "
            f"txc0={last_summary['txc_prod']}/{last_summary['txc_cons']}"
        )

    counters = data["counters"]
    if counters:
        print()
        print("Observed TX counters:")
        for name in sorted(counters):
            values = counters[name]
            print(f"  {name}: max={max(values)} samples={len(values)}")

    print()
    print("Next-step verdict:")
    if expected_release and not saw_expected:
        print("- Board did not run the expected FIT. Fix TFTP/boot path first.")
    elif unpadded_tx:
        print("- Short EDMA frames are still unpadded. Confirm the board loaded this build.")
    elif padded_tx and any(value & 0x40 for value in misc_nonzero):
        print("- hw_len=49 is active, but DATA_LEN_ERR still appears. Keep the full snapshot.")
    elif padded_tx and txcmpl_nonzero:
        print("- hw_len=49 is active and TX completion has errors. Decode txcmpl errors next.")
    elif padded_tx:
        print("- hw_len=49 is active. If ARP is still invisible, inspect port_tx/queue_tx/GMAC TX counters.")
    else:
        print("- No short EDMA TX evidence found. Capture ping plus nss_debug snapshot <port>.")

    if data["has_ping_fail"] and not data["has_snapshot"]:
        print("- The log has ping failure but no snapshot; run nss_debug snapshot 3 immediately after failure.")


def main():
    parser = argparse.ArgumentParser(description="Analyze SBE1V1K board network logs.")
    parser.add_argument("log", nargs="?", default="-", help="serial log file, or '-' for stdin")
    parser.add_argument(
        "--expect-release",
        default=repo_release(),
        help="expected U-Boot release token; defaults to include/config/uboot.release",
    )
    args = parser.parse_args()

    analyze(collect(read_log(args.log)), args.expect_release)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
