#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UBOOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX_SHIM="$SCRIPT_DIR/chainloader-prefix-shim.uImage"
SHIM="$SCRIPT_DIR/chainloader-shim.bin"
DTB="$SCRIPT_DIR/chainloader-control.dtb"
TEMPLATE="$SCRIPT_DIR/xr1710g-chainloader.its.in"

PAYLOAD="${1:-$UBOOT_DIR/u-boot.bin}"
OUTPUT_DIR="${2:-$UBOOT_DIR/out}"
MKIMAGE="${3:-$UBOOT_DIR/tools/mkimage}"
DUMPIMAGE="${4:-$UBOOT_DIR/tools/dumpimage}"

if [ ! -f "$PAYLOAD" ]; then
    echo "Error: payload not found: $PAYLOAD" >&2
    exit 1
fi

if [ ! -f "$MKIMAGE" ]; then
    echo "Error: mkimage not found: $MKIMAGE" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

OUTPUT_FIT="$OUTPUT_DIR/xr1710g-chainloader.itb"
OUTPUT_SLOT="$OUTPUT_DIR/xr1710g-chainloader-slot.bin"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

ITS="$TMPDIR/xr1710g-chainloader.its"

sed \
  -e "s|__DTB__|$DTB|g" \
  -e "s|__SHIM__|$SHIM|g" \
  -e "s|__PAYLOAD__|$PAYLOAD|g" \
  -e "s|__KCOMP__|none|g" \
  "$TEMPLATE" > "$ITS"

echo "Building FIT image..."
"$MKIMAGE" -f "$ITS" "$OUTPUT_FIT"
"$DUMPIMAGE" -l "$OUTPUT_FIT"

echo "Building slot image..."
prefix_size=$(wc -c < "$PREFIX_SHIM")
cp "$PREFIX_SHIM" "$OUTPUT_SLOT"
dd if=/dev/zero bs=1 count=$((0x2100 - prefix_size)) >> "$OUTPUT_SLOT" 2>/dev/null
cat "$OUTPUT_FIT" >> "$OUTPUT_SLOT"

slot_size=$(wc -c < "$OUTPUT_SLOT")
echo ""
echo "Done!"
echo "  FIT:  $OUTPUT_FIT ($(wc -c < "$OUTPUT_FIT") bytes)"
echo "  Slot: $OUTPUT_SLOT ($slot_size bytes)"
echo ""
echo "Magic check:"
echo "  Offset 0x0000: $(dd if="$OUTPUT_SLOT" bs=1 count=4 2>/dev/null | xxd -p)"
echo "  Offset 0x2100: $(dd if="$OUTPUT_SLOT" bs=1 skip=$((0x2100)) count=4 2>/dev/null | xxd -p)"
