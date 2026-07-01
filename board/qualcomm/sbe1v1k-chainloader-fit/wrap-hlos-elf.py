#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Wrap a FIT image in the ELF container expected by Askey IPQ eMMC HLOS."""

import argparse
import struct
from pathlib import Path


ELF_HEADER_SIZE = 0x34
ELF_PHDR_SIZE = 0x20
ELF_PHNUM = 3
FIT_OFFSET = 0x3000
FIT_LOAD_ADDR = 0x44000000


def elf32_header() -> bytes:
    ident = bytearray(16)
    ident[0:4] = b"\x7fELF"
    ident[4] = 1  # ELFCLASS32
    ident[5] = 1  # ELFDATA2LSB
    ident[6] = 1  # EV_CURRENT

    return struct.pack(
        "<16sHHIIIIIHHHHHH",
        bytes(ident),
        2,                # ET_EXEC
        3,                # EM_386, matches stock HLOS wrapper
        1,                # EV_CURRENT
        0,                # e_entry
        ELF_HEADER_SIZE,  # e_phoff
        0,                # e_shoff
        0,                # e_flags
        ELF_HEADER_SIZE,
        ELF_PHDR_SIZE,
        ELF_PHNUM,
        0,
        0,
        0,
    )


def phdr(p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
         p_flags, p_align) -> bytes:
    return struct.pack(
        "<IIIIIIII",
        p_type,
        p_offset,
        p_vaddr,
        p_paddr,
        p_filesz,
        p_memsz,
        p_flags,
        p_align,
    )


def build_wrapper(fit: bytes) -> bytes:
    header_len = ELF_HEADER_SIZE + ELF_PHDR_SIZE * ELF_PHNUM
    if header_len != 0x94:
        raise RuntimeError(f"unexpected ELF header length 0x{header_len:x}")

    out = bytearray()
    out += elf32_header()
    out += phdr(0, 0, 0, 0, header_len, 0, 0x07000000, 0)
    out += phdr(0, 0x1000, 0, 0, 0, 0, 0x02000000, 0x1000)
    out += phdr(1, FIT_OFFSET, FIT_LOAD_ADDR, FIT_LOAD_ADDR, len(fit),
                len(fit), 6, 0x1000)
    out += bytes(FIT_OFFSET - len(out))
    out += fit
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="input raw FIT")
    parser.add_argument("--output", required=True, help="output HLOS ELF")
    args = parser.parse_args()

    fit = Path(args.input).read_bytes()
    if fit[0:4] != b"\xd0\x0d\xfe\xed":
        raise SystemExit(f"{args.input}: not a FIT/FDT image")

    Path(args.output).write_bytes(build_wrapper(fit))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
