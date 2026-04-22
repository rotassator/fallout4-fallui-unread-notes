"""
Address Library binary parser (meh321 / F4SE flavour).

File layout: [u64 count][u64 id, u64 offset] * count
IDs are stable across game versions; offsets change per build.

OG (1.10.x pre-NG) DBs cover ~1.58M sequential IDs (exhaustive scan).
NG/AE DBs cover a curated ~600k subset, sharing the same ID space.
"""

import struct
import sys
from pathlib import Path

PLUGINS = Path(
    "C:/games/Vortex/fallout4/mods/"
    "Address Library - All In One-47327-1-11-191-1765967714/F4SE/Plugins"
)


def parse(path):
    data = path.read_bytes()
    (count,) = struct.unpack_from("<Q", data, 0)
    expected = 8 + count * 16
    if expected != len(data):
        raise ValueError(
            f"{path.name}: count {count} implies {expected} bytes, file is {len(data)}"
        )
    table = {}
    pairs = struct.unpack_from(f"<{count * 2}Q", data, 8)
    for i in range(count):
        table[pairs[i * 2]] = pairs[i * 2 + 1]
    return table


def load_all():
    bins = sorted(PLUGINS.glob("version-*.bin"))
    return {p.stem: parse(p) for p in bins}


def cmd_offset(dbs, target, db_stem):
    db = dbs[db_stem]
    hits = [i for i, off in db.items() if off == target]
    if not hits:
        print(f"No ID in {db_stem} maps to offset 0x{target:08X}")
        sorted_offs = sorted(db.items(), key=lambda kv: kv[1])
        below = [x for x in sorted_offs if x[1] < target][-3:]
        above = [x for x in sorted_offs if x[1] > target][:3]
        print("  Nearest below:")
        for i, off in below:
            print(f"    id={i:>7} offset=0x{off:08X}  (delta -0x{target-off:X})")
        print("  Nearest above:")
        for i, off in above:
            print(f"    id={i:>7} offset=0x{off:08X}  (delta +0x{off-target:X})")
        return
    for i in hits:
        print(f"ID {i} -> offset 0x{target:08X} in {db_stem}")
        print("  Resolves across all DBs:")
        for stem, d in dbs.items():
            v = d.get(i)
            print(f"    {stem}: " + (f"0x{v:08X}" if v is not None else "<missing>"))


def cmd_id(dbs, target_id):
    print(f"ID {target_id} across all DBs:")
    for stem, d in dbs.items():
        v = d.get(target_id)
        print(f"  {stem}: " + (f"0x{v:08X}" if v is not None else "<missing>"))


def cmd_coverage(dbs):
    print("ID coverage per DB (entry count):")
    for stem, d in dbs.items():
        print(f"  {stem}: {len(d):>8} IDs")
    og = dbs["version-1-10-163-0"]
    for stem in ["version-1-10-984-0", "version-1-11-191-0"]:
        cur = dbs[stem]
        shared = len(set(og) & set(cur))
        print(f"  {stem}: {shared:>8} IDs also in OG")


def main():
    dbs = load_all()
    if len(sys.argv) < 2:
        print("Usage:")
        print("  addrlib_lookup.py offset <hex_offset> [db_stem]")
        print("  addrlib_lookup.py id <id>")
        print("  addrlib_lookup.py coverage")
        return
    cmd = sys.argv[1]
    if cmd == "offset":
        target = int(sys.argv[2], 16)
        db_stem = sys.argv[3] if len(sys.argv) > 3 else "version-1-10-163-0"
        cmd_offset(dbs, target, db_stem)
    elif cmd == "id":
        cmd_id(dbs, int(sys.argv[2]))
    elif cmd == "coverage":
        cmd_coverage(dbs)


if __name__ == "__main__":
    main()
