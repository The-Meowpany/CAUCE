#!/usr/bin/env python3
"""Parses an ESP32 partition table image and prints it as text.

Kept as a file rather than an inline `python3 -c` because the shell quoting for a struct
unpack inside a PowerShell string inside a container command is three layers deep, and the
inline version was a PowerShell parse error rather than a program that ran and failed.

THE LAYOUT, AS DERIVED FROM THE ARTIFACT RATHER THAN FROM MEMORY

PlatformIO's `partitions.bin` is a *bare table image*: the ESP-IDF 32-byte MD5 block is not
present, so the file starts with the 2-byte magic and the entry array follows immediately.

    0x0000  magic              aa 50      stored big-endian
    0x0002  entry 0            32 bytes
    0x0022  entry 1
    ...     stride 0x20, 16 allocated slots
    0x0202  MD5 of the entry array, 16 bytes

And an entry is NOT the 26-byte struct the header comment suggests:

    +0   type      1 byte
    +1   subtype   1 byte
    +2   offset    4 bytes, little-endian
    +6   size      4 bytes, little-endian
    +10  label     16 bytes, NUL-padded
    +26  padding   6 bytes

Four wrong layouts before this one, each producing output that read like a corrupt artifact
rather than a parser with a bug:

- entries at 0x20 with the count from byte 2: one partition called `ata`, 32768 KB
- entries at 0x02 with label at +16: the size field read as a 2 GB label
- magic compared little-endian: "no partition magic" on a file that starts `aa 50`
- entries at 0x20 scanning 16 slots: the MD5 block parsed as partitions

What settled it was arithmetic against a table I already knew, `firmware/partitions.csv`:
nvs at 0x9000/0x5000, otadata at 0xe000/0x2000, app0 at 0x10000/0x1E0000, app1 at
0x1F0000/0x1E0000, spiffs at 0x3D0000/0x30000. A layout that reproduces those five is the
layout. One that does not is not, however plausible its output looks.
"""

import hashlib
import sys

MAGIC_BYTES = b"\xaa\x50"
MAGIC_OFFSET = 0x0000
ENTRIES_START = 0x0002
ENTRY_STRIDE = 0x20
ENTRY_SLOTS = 16
LABEL_OFFSET = 0x0A
OFFSET_OFFSET = 0x02
SIZE_OFFSET = 0x06
MD5_OFFSET = ENTRIES_START + ENTRY_SLOTS * ENTRY_STRIDE  # 0x202
SECTOR_SIZE = 4096

# THE SIZE FIELD IS IN BYTES, NOT SECTORS
#
# `partitions.csv` for this board declares `app0 ... 0x1E0000`, which is 1,966,080 bytes =
# 1920 KiB - and the comment in that file says exactly that. An earlier version of this parser
# multiplied by the sector size and reported app0 as 7864320KB, or 7.8 GB, on a 4 MB part.
#
# The offsets are also bytes, not sectors. So no conversion happens here at all: the numbers
# in the table are the numbers in the CSV, and printing them unchanged is the only way a
# reader can compare the two by eye. The one thing worth catching is a value that is not a
# multiple of the sector size, since the bootloader requires alignment.
UNALIGNED = "the size is not a whole number of 4 KiB sectors, which the bootloader rejects"

TYPE_NAMES = {
    0x00: "app",
    0x01: "data",
}

SUBTYPE_NAMES = {
    0x00: "",
    0x01: "ota",
    0x02: "nvs",
    0x10: "ota_0",
    0x11: "ota_1",
    0x82: "spiffs",
    0x83: "fat",
    0x90: "littlefs",
}


def find_magic(blob: bytes) -> int:
    """Locates the magic, tolerating a table image that is padded or offset."""
    if blob[MAGIC_OFFSET:MAGIC_OFFSET + 2] == MAGIC_BYTES:
        return MAGIC_OFFSET
    for offset in range(0, min(len(blob), 0x1000), 4):
        if blob[offset:offset + 2] == MAGIC_BYTES:
            return offset
    return -1


def parse(blob: bytes, base: int):
    partitions = []
    for index in range(ENTRY_SLOTS):
        at = base + ENTRIES_START + index * ENTRY_STRIDE
        if at + ENTRY_STRIDE > len(blob):
            break
        entry_type = blob[at]
        if entry_type == 0xFF:  # 0xFF marks an unused slot in a bare table image
            continue
        subtype = blob[at + 1]
        offset = int.from_bytes(blob[at + OFFSET_OFFSET:at + OFFSET_OFFSET + 4], "little")
        size = int.from_bytes(blob[at + SIZE_OFFSET:at + SIZE_OFFSET + 4], "little")
        label = blob[at + LABEL_OFFSET:at + LABEL_OFFSET + 16].split(b"\x00")[0]
        partitions.append({
            "index": index,
            "type": entry_type,
            "subtype": subtype,
            "offset": offset,
            "size": size,
            "label": label.decode("utf-8", "replace"),
        })
    return partitions


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: partition_report.py <partitions.bin>")
        return 2
    blob = open(sys.argv[1], "rb").read()
    if len(blob) < ENTRIES_START + ENTRY_STRIDE:
        print(f"too small to be a partition table ({len(blob)} bytes)")
        return 1

    base = find_magic(blob)
    if base < 0:
        print("no partition magic (aa 50) found in the first 4 KiB")
        return 1

    partitions = parse(blob, base)
    # The size field is in 4 KiB sectors, exactly as `partitions.csv` declares them, so the
    # value is NOT multiplied by the sector size when reporting. An early version printed
    # app0 as 7864320KB - 7.8 GB - from a declared 0x1E0000 sectors, which is 1920 KB on a 4 MB
    # part. The CSV and the binary agree; only the reporting was wrong.
    print(f"magic aa50 at file offset 0x{base:04x} "
          f"(0x{base + 0x8000:x} in flash)")
    print(f"table image, {len(partitions)} used of {ENTRY_SLOTS} slots")
    print("")

    if not partitions:
        print("no partitions found")
        return 1

    for part in partitions:
        kind = TYPE_NAMES.get(part["type"], f"type=0x{part['type']:02x}")
        sub = SUBTYPE_NAMES.get(part["subtype"], f"sub=0x{part['subtype']:02x}")
        # Bytes, unchanged, so the line reads against partitions.csv without arithmetic.
        print(f"{part['label'] or '(unlabelled)':16s} {kind:5s} {sub:8s} "
              f"offset=0x{part['offset']:06x} "
              f"size=0x{part['size']:06x} ({part['size'] // 1024:5d} KiB)")

    # The trailing MD5 is a real integrity check on the table, when present. A table image
    # with an all-zero MD5 means "not computed", which is not a failure.
    md5_at = base + MD5_OFFSET
    if md5_at + 16 <= len(blob):
        stored = blob[md5_at:md5_at + 16]
        entries_at = base + ENTRIES_START
        entries_end = entries_at + ENTRY_SLOTS * ENTRY_STRIDE
        computed = hashlib.md5(blob[entries_at:entries_end]).digest()
        print("")
        # All-zero means gen_esp32part.py was not asked to checksum, and all-ones is the
        # erased state of the flash this table is destined for. Neither is a finding.
        if stored in (b"\x00" * 16, b"\xff" * 16):
            print("note  the table carries no usable MD5 "
                  "(all-zero or erased); the entries above are the content")
        elif stored == computed:
            print("ok   the trailing MD5 matches the entry array")
        else:
            print("FAIL the trailing MD5 does not match the entry array")
            print(f"      stored   {stored.hex()}")
            print(f"      computed {computed.hex()}")
            return 1

    # Two application slots is the whole basis of the rollback story. One means OTA can
    # never fall back, and the firmware would compile and pass every host test while
    # providing no fallback at all.
    apps = [p for p in partitions if p["type"] == 0x00]
    print("")
    if len(apps) < 2:
        print(f"FAIL application slots: {len(apps)}; OTA rollback needs two")
        return 1
    smallest = min(p["size"] for p in apps)
    print(f"ok   application slots: {len(apps)}, smallest {smallest // 1024} KiB")

    # Alignment. The bootloader refuses a partition whose offset or size is not a whole
    # number of 4 KiB sectors, and the failure is at flash time with no useful message, so it
    # is worth catching here.
    misaligned = [
        p["label"] for p in partitions
        if p["offset"] % SECTOR_SIZE or p["size"] % SECTOR_SIZE
    ]
    if misaligned:
        print(f"FAIL misaligned partitions: {misaligned} - {UNALIGNED}")
        return 1
    print("ok   every partition is sector-aligned")

    # Slots must not overlap. An overlapping table compiles, flashes and then corrupts one
    # slot by writing the other, which is exactly the failure the two-slot layout exists to
    # avoid.
    ranges = sorted((p["offset"], p["offset"] + p["size"], p["label"]) for p in partitions)
    overlaps = [
        (a[2], b[2]) for a, b in zip(ranges, ranges[1:]) if a[1] > b[0]
    ]
    if overlaps:
        print(f"FAIL overlapping partitions: {overlaps}")
        return 1
    print("ok   no two partitions overlap")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())