#!/usr/bin/env python3
"""Build the raw Flash image consumed by src/rom_catalog.cpp."""

from __future__ import annotations

import argparse
import binascii
import csv
import struct
from pathlib import Path

MAGIC = b"NESPACK1"
VERSION = 1
HEADER_STRUCT = struct.Struct("<8sIIII8x")
ENTRY_STRUCT = struct.Struct("<40sIIIH10x")
SUPPORTED_MAPPERS = {
    0, 1, 2, 3, 4, 5, 7, 8, 9, 11, 15, 16, 18, 19, 21, 22, 23, 24,
    25, 32, 33, 34, 40, 64, 65, 66, 70, 75, 78, 79, 85, 94, 99, 231,
}


def ines_info(data: bytes, source: Path) -> tuple[int, int]:
    if len(data) < 16 or data[:4] != b"NES\x1a":
        raise ValueError(f"not an iNES ROM: {source}")
    mapper = data[6] >> 4
    reserved = data[8:16]
    disk_dude = data[7] == ord("D") and reserved == b"iskDude!"
    if reserved == bytes(8) or not disk_dude:
        mapper |= data[7] & 0xF0
    trainer = 512 if data[6] & 0x04 else 0
    expected = 16 + trainer + data[4] * 16384 + data[5] * 8192
    if len(data) < expected:
        raise ValueError(
            f"truncated iNES ROM: {source} ({len(data)} < {expected})"
        )
    if mapper not in SUPPORTED_MAPPERS:
        raise ValueError(f"mapper {mapper} is not enabled by Nofrendo: {source}")
    return mapper, expected


def build_pack(manifest: Path, rom_root: Path, output: Path,
               partition_size: int) -> tuple[int, int]:
    rows: list[tuple[str, Path, bytes, int]] = []
    with manifest.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            name = row["display_name"].strip()
            source = rom_root / row["source_file"].strip()
            encoded_name = name.encode("ascii")
            if not name or len(encoded_name) >= 40:
                raise ValueError(f"display name must be 1-39 ASCII bytes: {name!r}")
            data = source.read_bytes()
            mapper, _ = ines_info(data, source)
            rows.append((name, source, data, mapper))

    if not 1 <= len(rows) <= 60:
        raise ValueError(f"manifest must contain 1-60 games, got {len(rows)}")

    data_offset = HEADER_STRUCT.size + ENTRY_STRUCT.size * len(rows)
    data_offset = (data_offset + 15) & ~15
    entries = []
    payloads = []
    cursor = data_offset
    for name, source, data, mapper in rows:
        entries.append(ENTRY_STRUCT.pack(
            name.encode("ascii"), cursor, len(data),
            binascii.crc32(data) & 0xFFFFFFFF, mapper,
        ))
        payloads.append((cursor, data))
        cursor = (cursor + len(data) + 15) & ~15

    if cursor > partition_size:
        raise ValueError(
            f"ROM pack is {cursor} bytes but partition is {partition_size} bytes"
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as handle:
        handle.write(HEADER_STRUCT.pack(
            MAGIC, VERSION, len(rows), ENTRY_STRUCT.size, cursor
        ))
        for entry in entries:
            handle.write(entry)
        handle.write(b"\xFF" * (data_offset - handle.tell()))
        for offset, data in payloads:
            handle.write(b"\xFF" * (offset - handle.tell()))
            handle.write(data)
        handle.write(b"\xFF" * (cursor - handle.tell()))

    print(
        f"ROM pack: {len(rows)} games, {cursor} bytes "
        f"({cursor / 1048576:.2f} MiB), free "
        f"{(partition_size - cursor) / 1048576:.2f} MiB"
    )
    return len(rows), cursor


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--rom-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--partition-size", type=lambda value: int(value, 0),
                        default=0xE70000)
    args = parser.parse_args()
    build_pack(args.manifest, args.rom_root, args.output, args.partition_size)


if __name__ == "__main__":
    main()
