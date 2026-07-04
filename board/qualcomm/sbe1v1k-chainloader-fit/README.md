# SBE1V1K Chainloader FIT

This builds the stock-`bootm` compatible second-stage chainloader for the
Askey SBE1V1K.

The intended installation path starts from the factory 43-partition eMMC
layout. It does not use the 192.168.33.3/QWRT partition table. Factory
partitions p1-p26 are kept, including `0:HLOS` and `0:HLOS_1`; the chainloader
is never written to either HLOS slot.

## Serial Boot

Use only temporary U-Boot variables at the stock prompt. Do not run `saveenv`
from stock U-Boot.

```sh
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.2
tftpboot 0x80000000 sbe1v1k-chainloader.itb
bootm 0x80000000
```

The shim and recovery backend intentionally look for the running raw FIT at
`0x80000000` only.

The FIT embeds the real second-stage U-Boot as `uboot-1`, but `conf-1` does
not list it as a FIT `loadables` entry. On the SBE1V1K stock U-Boot, preloading
that payload to the second-stage address can overwrite the still-running stock
loader before it jumps to the shim. The shim copies `uboot-1` from the raw FIT
itself after stock `bootm` has transferred control.

If the system TFTP root is not writable, the helper script in this directory
can serve the generated FIT from an unprivileged port:

```sh
board/qualcomm/sbe1v1k-chainloader-fit/serve-chainloader-tftp.py \
	--host 0.0.0.0 \
	--port 6969
```

At startup the helper prints the served FIT path, size, SHA256, and embedded
U-Boot version banner. It also checks `/srv/tftp/sbe1v1k-chainloader.itb` when
serving from a non-default port and warns if stock U-Boot's default TFTP port 69
would see a missing or stale copy. Check the printed hash and banner before
booting from stock U-Boot. The helper also prints the exact temporary stock
U-Boot commands for the selected port.

For the default helper port, those commands are:

```sh
setenv tftpdstp 6969
tftpboot 0x80000000 sbe1v1k-chainloader.itb
setenv tftpdstp
bootm 0x80000000
```

Do not save the environment while using the temporary high-port TFTP path.

## Board Log Analysis

Use the log analyzer on captured serial output before changing EDMA defaults
based on a failed ping:

```sh
board/qualcomm/sbe1v1k-chainloader-fit/analyze-board-log.py board.log
```

By default it compares the log against `include/config/uboot.release`, reports
whether the expected U-Boot banner was seen, checks if short EDMA TX frames were
padded to `hw_len=49`, decodes EDMA misc bits such as `DATA_LEN_ERR` and
`TX_TIMEOUT`, and summarizes TX completion errors and PPE/GMAC TX counters when
they are present in the log.

## HTTP Migration

Open the recovery page at:

```text
http://192.168.255.1/
```

Use `Repartition factory eMMC` and enter:

```text
SBE1V1K_REPARTITION
```

The recovery action:

- verifies the current GPT matches key factory 43-partition locations;
- rewrites only p27 and later;
- keeps p1-p26 untouched, including `0:HLOS` and `0:HLOS_1`;
- creates `chainloader`, `kernel`, `rootfs`, and `rootfs_data`;
- writes the currently running FIT from `0x80000000` into `chainloader`;
- updates `0:APPSBLENV` with `fw_setenv`-equivalent variable changes;
- keeps the HTTP server running so firmware can be uploaded next.

The installed stock U-Boot environment is equivalent to:

```sh
bootargs='console=ttyMSM0,115200n8 rootwait root=PARTLABEL=rootfs'
boot_chainloader='mmc read 0x44000000 0x0001b022 0x2000; bootm 0x44000000'
do_boot='run boot_chainloader'
bootcmd='echo "Hit ctrl+c for shell..."; if sleep 3; then run do_boot; else true; fi;'
```

Other factory U-Boot environment variables are preserved.
The Ethernet MAC fallback also reads `ethaddr` from `0:APPSBLENV` when the
runtime environment does not provide one.

## Target Layout

Sector size is 512 bytes.

| No. | Start | Sectors | Size | Name | Purpose |
| ---: | ---: | ---: | ---: | --- | --- |
| 1-26 | unchanged | unchanged | unchanged | factory labels | boot chain, ART, ETHPHYFW, WIFIFW, HLOS/HLOS_1 |
| 27 | 110626 (`0x1b022`) | 8192 (`0x2000`) | 4 MiB | `chainloader` | raw `sbe1v1k-chainloader.itb` |
| 28 | 118818 (`0x1d022`) | 65536 (`0x10000`) | 32 MiB | `kernel` | OpenWrt/QSDK kernel FIT |
| 29 | 184354 (`0x2d022`) | 2097152 (`0x200000`) | 1 GiB | `rootfs` | OpenWrt/QSDK root image |
| 30 | 2281506 (`0x22d022`) | to last usable sector | about 6.2 GiB | `rootfs_data` | persistent data |

The new layout leaves room for a valid secondary GPT.

## Recovery Targets

The chainloader default environment writes:

```text
uboot:        0#chainloader
firmware FIT: 0#kernel + 0#rootfs
root data:    0#rootfs_data
```

OpenWrt/QSDK boots from the second-stage chainloader via `kernel` and
`root=PARTLABEL=rootfs`. It does not depend on `0:HLOS`.

## Build

```sh
make sbe1v1k_chainloader_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
board/qualcomm/sbe1v1k-chainloader-fit/build-chainloader-fit.sh \
	--payload u-boot.bin \
	--outdir sbe1v1k-chainloader
```

Default output:

- `sbe1v1k-chainloader/sbe1v1k-chainloader.itb`
- `sbe1v1k-chainloader/sbe1v1k-chainloader-hlos.elf`
- `sbe1v1k-chainloader/sbe1v1k-chainloader-shim.bin`
- `sbe1v1k-chainloader/sbe1v1k-chainloader-control.dtb`

The build script prints SHA256 values for these artifacts and the embedded
U-Boot banner found in the generated raw FIT. Check that banner before copying
or serving the FIT.

The `*-hlos.elf` wrapper is generated only for inspection and experiments with
the original Askey loader format. The normal chainloader path is the raw FIT.
