# SBE1V1K Chainloader FIT

This builds the stock-`bootm` compatible second-stage chainloader for the
Askey SBE1V1K.

The intended installation path starts from an SBE1V1K eMMC GPT layout. It does
not use the 192.168.33.3/QWRT partition table. Recovery preserves all existing
partitions that end before LBA `0x1b022`, including the boot chain, `0:ART`,
`0:WIFIFW`, and `0:HLOS`; the chainloader is never written to an HLOS slot.
Some QSDK variants omit `0:HLOS_1`, so partition numbers after `0:HLOS` are not
assumed to be stable.

## Serial Boot

Use only temporary U-Boot variables at the stock prompt. Do not run `saveenv`
from stock U-Boot.

```sh
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.2
tftpboot 0x80000000 sbe1v1k-chainloader.itb
bootm 0x80000000
```

The TFTP path loads the raw FIT at `0x80000000`. The persistent eMMC path loads
the installed FIT at `0x44000000`, and the shim searches both addresses.

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

- verifies key boot-chain anchors before the chainloader area;
- rewrites only the area starting at LBA `0x1b022`;
- keeps all existing partitions before LBA `0x1b022` untouched;
- creates `chainloader`, `kernel`, `rootfs`, and `rootfs_data`;
- writes the currently running FIT from `0x80000000` into `chainloader`;
- updates `0:APPSBLENV` with `fw_setenv`-equivalent variable changes and
  verifies them by reading the partition back;
- keeps the HTTP server running so firmware can be uploaded next.

SBE1V1K is an eMMC/GPT device. Recovery writes raw GPT partitions only; it must
not format, create, resize, or write UBI volumes on this board.

The installed stock U-Boot environment is equivalent to:

```sh
bootargs='console=ttyMSM0,115200n8 rootwait root=PARTLABEL=rootfs'
boot_chainloader='mmc dev 0 0; mmc read 0x44000000 0x0001b022 0x2000; bootm 0x44000000'
do_boot='run boot_chainloader'
do_nothing='true'
bootcmd='echo "Hit ctrl+c for shell..."; if sleep 3; then setenv bootargs console=ttyMSM0,115200n8 rootwait root=PARTLABEL=rootfs; run do_boot; else run do_nothing; fi;'
```

`bootcmd` sets `bootargs` again on every boot because the vendor U-Boot resets
it during startup. Other factory U-Boot environment variables are preserved.
The Ethernet MAC fallback also reads `ethaddr` from `0:APPSBLENV` when the
runtime environment does not provide one.

## Target Layout

Sector size is 512 bytes.

| Start | Sectors | Size | Name | Purpose |
| ---: | ---: | ---: | --- | --- |
| before 110626 (`0x1b022`) | unchanged | unchanged | existing labels | boot chain, ART, ETHPHYFW, WIFIFW, HLOS, optional HLOS_1 |
| 110626 (`0x1b022`) | 8192 (`0x2000`) | 4 MiB | `chainloader` | raw `sbe1v1k-chainloader.itb` |
| 118818 (`0x1d022`) | 65536 (`0x10000`) | 32 MiB | `kernel` | OpenWrt/QSDK kernel FIT |
| 184354 (`0x2d022`) | 2097152 (`0x200000`) | 1 GiB | `rootfs` | OpenWrt/QSDK root image |
| 2281506 (`0x22d022`) | to last usable sector | about 6.2 GiB | `rootfs_data` | persistent data |

The new layout leaves room for a valid secondary GPT.
The numeric partition index of `chainloader` depends on the source GPT. On
standard factory images it follows `0:HLOS_1`; on QSDK variants without
`0:HLOS_1`, it follows `0:HLOS`. Scripts and recovery code use labels and fixed
LBAs, not the numeric index.

## Recovery Targets

The chainloader default environment writes:

```text
uboot:        0#chainloader
firmware FIT: 0#kernel + 0#rootfs
root data:    0#rootfs_data
```

OpenWrt/QSDK boots from the second-stage chainloader via `kernel` and
`root=PARTLABEL=rootfs`. It does not depend on `0:HLOS`.

## Offline eMMC Recovery

If the installed chainloader hangs before the second-stage U-Boot banner and
the stock U-Boot shell cannot be reached, rewrite only partition 27 on the eMMC
user area. Do not rewrite the full device and do not use the eMMC boot0/boot1
hardware partitions.

Expected chainloader partition:

```text
partition: chainloader
start LBA: 110626 (0x1b022)
sectors:   8192   (0x2000)
offset:    0x3604400
size:      4 MiB
```

After attaching the eMMC to a Linux host, first confirm the device and GPT:

```sh
sudo sgdisk -p /dev/sdX
```

The GPT should show partition 27 named `chainloader` at start sector `110626`.
If the host creates a partition node, write the padded 4 MiB chainloader image
to that partition:

```sh
sudo dd if=sbe1v1k-chainloader/sbe1v1k-chainloader-partition.img \
	of=/dev/sdX27 bs=4M conv=fsync
```

If a partition node is not available, write the same image at the fixed byte
offset on the whole eMMC user device:

```sh
sudo dd if=sbe1v1k-chainloader/sbe1v1k-chainloader-partition.img \
	of=/dev/sdX bs=512 seek=110626 conv=notrunc,fsync
```

For `/dev/mmcblkN`, the partition node form is `/dev/mmcblkNp27`. Replace
`/dev/sdX` with the actual eMMC user-area device.

If a full-device rewrite is unavoidable, first make a raw backup of the eMMC
user area and, when exposed by the reader, both eMMC boot areas:

```sh
sudo dd if=/dev/sdX of=sbe1v1k-emmc-user-before.img \
	bs=4M conv=sync,noerror status=progress
sudo dd if=/dev/mmcblkNboot0 of=sbe1v1k-emmc-boot0-before.img \
	bs=4M conv=sync,noerror status=progress
sudo dd if=/dev/mmcblkNboot1 of=sbe1v1k-emmc-boot1-before.img \
	bs=4M conv=sync,noerror status=progress
```

The factory layout has known integration problems that this chainloader flow
intentionally replaces: the tail partition can invalidate the backup GPT, the
stock environment does not boot the new GPT layout, and the stock loader cannot
directly load the embedded second-stage U-Boot payload safely. Those are not
reasons to overwrite board-unique data.

For whole-device recovery, preserve these partitions from the same board unless
there is a verified same-unit backup:

```text
0:APPSBLENV  environment, fallback MAC variables
0:ART        board calibration and MAC-related data
0:LICENSE    device license data
```

The boot-chain partitions before `chainloader` can be restored from a trusted
same-model factory image only when they are known bad. Do not use another
unit's `0:ART`, `0:APPSBLENV`, or `0:LICENSE` as a generic replacement.

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
