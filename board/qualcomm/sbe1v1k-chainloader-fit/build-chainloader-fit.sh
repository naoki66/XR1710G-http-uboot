#!/usr/bin/env sh
set -eu

usage() {
	cat <<'EOF'
Usage:
  build-chainloader-fit.sh \
    [--payload <u-boot.bin>] \
    [--outdir <dir>] \
    [--cc <aarch64-gcc>] \
    [--objcopy <aarch64-objcopy>] \
    [--dtc <dtc>] \
	[--mkimage <mkimage>] \
	[--dumpimage <dumpimage>]

Builds a stock-bootm compatible SBE1V1K raw chainloader FIT. An Askey HLOS
ELF wrapper is also emitted for inspection, but it is not the default install
format.
EOF
}

payload=
outdir=
cc="${CC:-aarch64-linux-gnu-gcc}"
objcopy="${OBJCOPY:-aarch64-linux-gnu-objcopy}"
dtc="${DTC:-dtc}"
mkimage=
dumpimage=

while [ "$#" -gt 0 ]; do
	case "$1" in
		--payload) payload="$2"; shift 2 ;;
		--outdir) outdir="$2"; shift 2 ;;
		--cc) cc="$2"; shift 2 ;;
		--objcopy) objcopy="$2"; shift 2 ;;
		--dtc) dtc="$2"; shift 2 ;;
		--mkimage) mkimage="$2"; shift 2 ;;
		--dumpimage) dumpimage="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
	esac
done

abspath() {
	dir=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)
	base=$(basename -- "$1")
	printf '%s/%s\n' "$dir" "$base"
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
srctree=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
objtree="$srctree"

[ -n "$payload" ] || payload="$objtree/u-boot.bin"
[ -n "$outdir" ] || outdir="$objtree/sbe1v1k-chainloader"
[ -n "$mkimage" ] || mkimage="$objtree/tools/mkimage"
[ -n "$dumpimage" ] || dumpimage="$objtree/tools/dumpimage"

cc_path=$(command -v "$cc" 2>/dev/null || true)
objcopy_path=$(command -v "$objcopy" 2>/dev/null || true)
dtc_path=$(command -v "$dtc" 2>/dev/null || true)
[ -n "$dtc_path" ] || dtc_path="$objtree/scripts/dtc/dtc"

[ -n "$cc_path" ] || { echo "Compiler not found: $cc" >&2; exit 1; }
[ -n "$objcopy_path" ] || { echo "Objcopy not found: $objcopy" >&2; exit 1; }
[ -x "$dtc_path" ] || { echo "dtc not executable: $dtc_path" >&2; exit 1; }
[ -x "$mkimage" ] || { echo "mkimage not executable: $mkimage" >&2; exit 1; }
[ -x "$dumpimage" ] || { echo "dumpimage not executable: $dumpimage" >&2; exit 1; }
[ -f "$payload" ] || { echo "Missing payload: $payload" >&2; exit 1; }

mkdir -p "$outdir"
payload=$(abspath "$payload")

shim_elf="$outdir/sbe1v1k-chainloader-shim.elf"
shim_bin="$outdir/sbe1v1k-chainloader-shim.bin"
shim_map="$outdir/sbe1v1k-chainloader-shim.map"
control_dtb="$outdir/sbe1v1k-chainloader-control.dtb"
output_fit="$outdir/sbe1v1k-chainloader.itb"
output_hlos="$outdir/sbe1v1k-chainloader-hlos.elf"

cflags="
	-Os
	-march=armv8-a
	-ffreestanding
	-fno-builtin
	-fno-stack-protector
	-fno-asynchronous-unwind-tables
	-fno-unwind-tables
	-fno-pic
	-nostdlib
	-nostartfiles
	-nodefaultlibs
	-Wall
	-Wextra
	-Werror
	-I$srctree/scripts/dtc/libfdt
"

"$cc_path" $cflags \
	-T "$script_dir/shim/sbe1v1k-chainloader.lds" \
	-Wl,-Map,"$shim_map" \
	-Wl,--build-id=none \
	-o "$shim_elf" \
	"$script_dir/shim/start.S" \
	"$script_dir/shim/main.c" \
	"$script_dir/shim/runtime.c" \
	"$srctree/scripts/dtc/libfdt/fdt.c" \
	"$srctree/scripts/dtc/libfdt/fdt_ro.c"

"$objcopy_path" -O binary "$shim_elf" "$shim_bin"
"$dtc_path" -I dts -O dtb -o "$control_dtb" \
	"$script_dir/shim/sbe1v1k-chainloader-control.dts"

shim_bin=$(abspath "$shim_bin")
control_dtb=$(abspath "$control_dtb")

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT INT TERM
its="$tmpdir/sbe1v1k-chainloader.its"

sed \
	-e "s|__DTB__|$control_dtb|g" \
	-e "s|__SHIM__|$shim_bin|g" \
	-e "s|__PAYLOAD__|$payload|g" \
	"$script_dir/sbe1v1k-chainloader.its.in" > "$its"

"$mkimage" -f "$its" "$output_fit"
"$dumpimage" -l "$output_fit"
python3 "$script_dir/wrap-hlos-elf.py" \
	--input "$output_fit" \
	--output "$output_hlos"

echo
echo "Generated FIT:  $output_fit"
echo "Generated HLOS: $output_hlos"
echo "Generated shim: $shim_bin"
echo "Generated DTB:  $control_dtb"
