#!/usr/bin/env python3
"""Build the merged flash image used by the simulator (and for rescue flashes).

`idf.py build` refreshes the app binary but never re-merges the full image, so
the simulator kept booting whatever full.bin was lying around from an earlier
session.  This script owns that step.

    python tools/mk_full_bin.py                     # -> build/FoloToy-AI-Passport-full.bin
    python tools/mk_full_bin.py --out foo.bin       # somewhere else
    python tools/mk_full_bin.py --rom 0=main/roms/digimon.bin:V1 \
                                --rom 1=main/roms/digimonv2.bin:V2

`--rom SLOT=FILE[:NAME]` writes a ROM into the 'roms' partition of the image
with the same slot layout the firmware uses (dg_rom_store.h): a 4 KiB header
sector followed by the 16 KiB payload, 32 KiB per slot.  The device does not
need this -- it takes ROMs over USB -- but a pre-filled image lets the
simulator boot straight into the game and lets a rescue flash carry a ROM.
"""

import argparse
import json
import pathlib
import struct
import zlib

ROM_MAGIC = 0x31524744  # "DGR1"
ROM_SLOT_STRIDE = 0x8000
ROM_DATA_OFF = 0x1000
ROM_NAME_LEN = 12
ROM_MAX_LEN = 16384

DEFAULT_BUILD = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "digimon-passport" / "build"


def rom_header(payload: bytes, name: str, seq: int = 0) -> bytes:
    if len(payload) > ROM_MAX_LEN:
        raise SystemExit(f"ROM is {len(payload)} B, the slot holds {ROM_MAX_LEN}")
    raw_name = name.encode("ascii", "replace")[: ROM_NAME_LEN - 1]
    head = struct.pack("<IIII", ROM_MAGIC, len(payload), zlib.crc32(payload) & 0xFFFFFFFF, seq)
    head += raw_name.ljust(ROM_NAME_LEN, b"\x00")
    return head + struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(DEFAULT_BUILD))
    ap.add_argument("--out", default=None)
    ap.add_argument("--rom", action="append", default=[], metavar="SLOT=FILE[:NAME]")
    args = ap.parse_args()

    build = pathlib.Path(args.build)
    flash_args = build / "flasher_args.json"
    if not flash_args.exists():
        raise SystemExit(f"{flash_args} missing -- run build_local.ps1 first")
    meta = json.loads(flash_args.read_text(encoding="utf-8"))

    blobs = []
    # flasher_args.json maps offset -> path ("0x10000": "FoloToy-AI-Passport.bin")
    for off, name in meta["flash_files"].items():
        blobs.append((int(off, 16), (build / name).read_bytes()))

    roms_partition = None
    table = None
    for off, data in blobs:
        if off == 0x8000:
            table = data
    if table is None:
        raise SystemExit("partition table blob not found in flasher_args.json")
    for i in range(0, len(table), 32):
        entry = table[i : i + 32]
        if len(entry) < 32 or entry[0:2] != b"\xaa\x50":
            break
        _, ptype, psub, offset, size = struct.unpack("<HBBII", entry[:12])
        label = entry[12:28].rstrip(b"\x00").decode()
        if label == "roms":
            roms_partition = (offset, size)

    rom_images = []
    for spec in args.rom:
        slot_s, rest = spec.split("=", 1)
        slot = int(slot_s, 10)
        if ":" in rest:
            path_s, name = rest.rsplit(":", 1)
        else:
            path_s, name = rest, f"V{slot + 1}"
        payload = pathlib.Path(path_s).read_bytes()
        rom_images.append((slot, payload, name))
    if rom_images and roms_partition is None:
        raise SystemExit("no 'roms' partition in the built table -- check partitions.csv / sdkconfig CUSTOM")

    # Pre-fill the start of the ROM area: the emulator maps the partition even
    # when the image is shorter than it, and a defined 0xFF run keeps the slot
    # scan honest.  1 MiB covers 32 slots; the full 6.1 MiB would bloat the file.
    if roms_partition:
        base, size = roms_partition
        blobs.append((base, b"\xff" * min(size, 0x100000)))

    for slot, payload, name in rom_images:
        base, size = roms_partition
        slot_off = base + slot * ROM_SLOT_STRIDE
        if slot * ROM_SLOT_STRIDE + ROM_DATA_OFF + len(payload) > size:
            raise SystemExit(f"slot {slot} does not fit the roms partition")
        blobs.append((slot_off, rom_header(payload, name)))
        blobs.append((slot_off + ROM_DATA_OFF, payload.ljust(ROM_MAX_LEN, b"\xff")))

    # later blobs overlay earlier ones, exactly like esptool merge_bin does
    blobs.sort(key=lambda b: b[0])
    end = max(off + len(data) for off, data in blobs)
    image = bytearray(b"\xff" * end)
    for off, data in blobs:
        image[off : off + len(data)] = data

    out = pathlib.Path(args.out) if args.out else build / "FoloToy-AI-Passport-full.bin"
    out.write_bytes(image)
    print(f"{out}  {len(image)} B")
    for slot, payload, name in rom_images:
        print(f"  rom slot {slot}: {name} {len(payload)} B crc {zlib.crc32(payload) & 0xFFFFFFFF:08x}")
    if roms_partition:
        print(f"  roms partition @0x{roms_partition[0]:06x} size 0x{roms_partition[1]:06x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
