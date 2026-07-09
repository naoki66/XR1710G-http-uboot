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
TXDESC_INIT_RE = re.compile(
    r"EDMA TXDESC(?P<ring>\d+) init "
    r"prod=(?P<prod_local>\d+)/(?P<prod_raw>\d+) "
    r"cons=(?P<cons_local>\d+)/(?P<cons_raw>\d+)"
)
RXDESC_INIT_RE = re.compile(
    r"EDMA RXDESC(?P<ring>\d+) init "
    r"cons=(?P<cons>\d+) prod=(?P<prod>\d+)"
)
RXFILL_PRIME_RE = re.compile(
    r"EDMA RXFILL(?P<ring>\d+) prime "
    r"cons=(?P<cons_local>\d+)/(?P<cons_raw>\d+) "
    r"prod=(?P<prod_local>\d+)/(?P<prod_raw>\d+)"
)
COUNTER_PACKETS_RE = re.compile(
    r"\b(?P<name>port_tx|queue_tx|ppe_port_tx|ppe_queue_tx)\b.*?"
    r"packets=(?P<packets>\d+)"
)
GMAC_TX_RE = re.compile(r"\bgmac_tx_mib:.*?txbytes=0x(?P<txbytes>[0-9a-fA-F]+)")
PPE_TX_PATH_RE = re.compile(
    r"\bppe_tx_path:.*?txbytes=0x(?P<txbytes>[0-9a-fA-F]+)"
)
PING_ALIVE_RE = re.compile(r"\bhost\s+(?P<host>\S+)\s+is alive\b")
PHY_LINK_RE = re.compile(
    r"\bPHY(?P<port>\d+)\s+(?P<link>Up|Down)\s+Speed\s*:\s*"
    r"(?P<speed>\d+)\s+(?P<duplex>Full|Half)\s+duplex"
)
SNAPSHOT_PORT_RE = re.compile(
    r"NSS snapshot:\s+requested_port=(?P<port>\d+)\s+requested_queue=(?P<queue>\d+)"
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

    txdesc_init = [
        {key: int(value) for key, value in match.groupdict().items()}
        for match in TXDESC_INIT_RE.finditer(text)
    ]
    rxdesc_init = [
        {key: int(value) for key, value in match.groupdict().items()}
        for match in RXDESC_INIT_RE.finditer(text)
    ]
    rxfill_prime = [
        {key: int(value) for key, value in match.groupdict().items()}
        for match in RXFILL_PRIME_RE.finditer(text)
    ]

    counters = {}
    for match in COUNTER_PACKETS_RE.finditer(text):
        name = match.group("name")
        counters.setdefault(name, []).append(int(match.group("packets")))
    for match in GMAC_TX_RE.finditer(text):
        counters.setdefault("gmac_tx_bytes", []).append(int(match.group("txbytes"), 16))
    for match in PPE_TX_PATH_RE.finditer(text):
        counters.setdefault("ppe_tx_path_bytes", []).append(int(match.group("txbytes"), 16))

    phy_links = []
    for match in PHY_LINK_RE.finditer(text):
        phy_links.append(
            {
                "port": int(match.group("port")),
                "link": match.group("link"),
                "speed": int(match.group("speed")),
                "duplex": match.group("duplex"),
            }
        )

    snapshot_ports = [
        int(match.group("port")) for match in SNAPSHOT_PORT_RE.finditer(text)
    ]

    return {
        "banners": banners,
        "tx_lines": tx_lines,
        "misc_values": misc_values,
        "txcmpl_errors": txcmpl_errors,
        "summaries": summaries,
        "txdesc_init": txdesc_init,
        "rxdesc_init": rxdesc_init,
        "rxfill_prime": rxfill_prime,
        "counters": counters,
        "phy_links": phy_links,
        "snapshot_ports": snapshot_ports,
        "alive_hosts": PING_ALIVE_RE.findall(text),
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
    latest_phy_links = {}
    for entry in data["phy_links"]:
        latest_phy_links[entry["port"]] = entry
    up_ports = sorted(
        port for port, entry in latest_phy_links.items() if entry["link"] == "Up"
    )
    recommended_port = up_ports[0] if len(up_ports) == 1 else 5

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

    if data["txdesc_init"] or data["rxdesc_init"] or data["rxfill_prime"]:
        print()
        print("EDMA ring init evidence:")
        for entry in data["rxdesc_init"]:
            print(
                f"  RXDESC{entry['ring']} init "
                f"cons={entry['cons']} prod={entry['prod']}"
            )
        for entry in data["rxfill_prime"]:
            print(
                f"  RXFILL{entry['ring']} prime "
                f"cons={entry['cons_local']}/{entry['cons_raw']} "
                f"prod={entry['prod_local']}/{entry['prod_raw']}"
            )
        for entry in data["txdesc_init"]:
            print(
                f"  TXDESC{entry['ring']} init "
                f"prod={entry['prod_local']}/{entry['prod_raw']} "
                f"cons={entry['cons_local']}/{entry['cons_raw']}"
            )

    counters = data["counters"]
    if counters:
        print()
        print("Observed TX counters:")
        for name in sorted(counters):
            values = counters[name]
            print(f"  {name}: max={max(values)} samples={len(values)}")

    if data["alive_hosts"]:
        print()
        print_list("Ping success observed:", data["alive_hosts"])

    if latest_phy_links:
        print()
        print("Latest PHY link state:")
        for port in sorted(latest_phy_links):
            entry = latest_phy_links[port]
            print(
                f"  port{port}: {entry['link']} {entry['speed']} "
                f"{entry['duplex']} duplex"
            )

    if data["snapshot_ports"]:
        print()
        print_list("Snapshot PPE ports:", data["snapshot_ports"])

    print()
    print("Next-step verdict:")
    if expected_release and not saw_expected:
        print("- Board did not run the expected FIT. Fix TFTP/boot path first.")
    elif data["alive_hosts"]:
        print("- Ping succeeded. EDMA reset/index, TX completion, RX, and GMAC egress are working for this port.")
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
        print(
            "- The log has ping failure but no snapshot; run "
            f"nss_debug snapshot {recommended_port} immediately after failure."
        )
    elif data["has_ping_fail"] and up_ports and data["snapshot_ports"]:
        wrong_ports = sorted(
            set(port for port in data["snapshot_ports"] if port not in up_ports)
        )
        if wrong_ports:
            print(
                "- Snapshot port does not match the latest linked PHY; "
                f"linked={','.join(map(str, up_ports))} "
                f"captured={','.join(map(str, wrong_ports))}."
            )


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
