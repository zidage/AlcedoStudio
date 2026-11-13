#!/usr/bin/env python3
"""Build the Nikon lens id map JSON from the ExifTool and Exiv2 Nikon lens tables.

Nikon RAW files do not store a lens name for most lenses. LibRaw reports the lens identity as
numbers, and the metadata extractor turns these numbers into the name that Lensfun matches:

- F-mount and 1-mount lenses: the 8-byte composite LensID. The keys of ExifTool's
  %nikonLensIDs table in lib/Image/ExifTool/Nikon.pm use the same byte order.
- Native Z-mount lenses: the 16-bit LensID of the new LensData. ExifTool lists these IDs in the
  PrintConv of the LensData 0x30 'LensID' tag.

ExifTool is the primary source. An Exiv2 checkout (src/nikonmn_int.cpp, table
nikonZMountLensId) can be given to add Z-mount IDs that ExifTool does not list yet. Exiv2 names
start with "Nikon "; this prefix is removed so that the names use the ExifTool spelling.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

EXIFTOOL_HEX_ENTRY_RE = re.compile(
    r"^    '(?P<key>[0-9A-F]{2}(?: [0-9A-F]{2}){7})(?:\.\d+)?' => '(?P<name>[^']*)',"
)
EXIFTOOL_Z_ENTRY_RE = re.compile(r"^\s+(?P<key>\d+) => '(?P<name>[^']*)',")
EXIV2_Z_ENTRY_RE = re.compile(r'^\s+\{(?P<key>\d+), "(?P<name>[^"]*)"\},')


def merge_name(names: dict[str, str], key: str, name: str) -> None:
    """Join different names for one key with " | ", as ExifTool lists alternatives."""
    name = " ".join(name.split())
    existing = names.get(key)
    if existing is None:
        names[key] = name
    elif name not in existing.split(" | "):
        names[key] = f"{existing} | {name}"


def extract_block(lines: list[str], start_pattern: str, end_pattern: str) -> list[str]:
    start = next((i for i, line in enumerate(lines) if re.search(start_pattern, line)), None)
    if start is None:
        raise SystemExit(f"Start of table not found: {start_pattern}")
    for end in range(start + 1, len(lines)):
        if re.search(end_pattern, lines[end]):
            return lines[start + 1 : end]
    raise SystemExit(f"End of table not found after: {start_pattern}")


def read_exiftool_tables(nikon_pm: Path) -> tuple[dict[str, str], dict[str, str]]:
    lines = nikon_pm.read_text(encoding="utf-8").splitlines()

    hex_names: dict[str, str] = {}
    for line in extract_block(lines, r"^%nikonLensIDs = \($", r"^\);$"):
        match = EXIFTOOL_HEX_ENTRY_RE.match(line)
        if match:
            merge_name(hex_names, match.group("key"), match.group("name"))

    # The Z-mount table is the PrintConv of the LensData tag 0x30 named 'LensID'.
    z_tag_start = next(
        (
            i
            for i in range(len(lines) - 1)
            if re.match(r"^\s+0x30 => \{", lines[i]) and "Name => 'LensID'" in lines[i + 1]
        ),
        None,
    )
    if z_tag_start is None:
        raise SystemExit("ExifTool LensData 0x30 'LensID' tag not found")
    z_lines = extract_block(lines[z_tag_start:], r"PrintConv => \{", r"^\s+\},$")
    z_names: dict[str, str] = {}
    for line in z_lines:
        match = EXIFTOOL_Z_ENTRY_RE.match(line)
        if match:
            merge_name(z_names, str(int(match.group("key"))), match.group("name"))

    if not hex_names or not z_names:
        raise SystemExit(f"No lens entries were read from {nikon_pm}")
    return hex_names, z_names


def read_exiv2_z_table(nikonmn_int: Path) -> dict[str, str]:
    lines = nikonmn_int.read_text(encoding="utf-8").splitlines()
    z_names: dict[str, str] = {}
    for line in extract_block(lines, r"TagDetails nikonZMountLensId\[\] = \{", r"^\};$"):
        match = EXIV2_Z_ENTRY_RE.match(line)
        if not match or match.group("key") == "0":
            continue
        name = match.group("name")
        if name.startswith("Nikon "):
            name = name[len("Nikon ") :]
        z_names[str(int(match.group("key")))] = " ".join(name.split())
    if not z_names:
        raise SystemExit(f"No Z-mount lens entries were read from {nikonmn_int}")
    return z_names


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exiftool-nikon-pm", type=Path, required=True,
                        help="ExifTool lib/Image/ExifTool/Nikon.pm.")
    parser.add_argument("--exiftool-version", required=True,
                        help="ExifTool release of the Nikon.pm file, for example 13.59.")
    parser.add_argument("--exiv2-nikonmn-int", type=Path,
                        help="Exiv2 src/nikonmn_int.cpp, for Z-mount IDs missing in ExifTool.")
    parser.add_argument("--exiv2-revision",
                        help="Exiv2 commit of the nikonmn_int.cpp file.")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("alcedo_studio/src/config/nikon_lens/id_map.json"),
        help="Output JSON file path.",
    )
    args = parser.parse_args()
    if (args.exiv2_nikonmn_int is None) != (args.exiv2_revision is None):
        parser.error("--exiv2-nikonmn-int and --exiv2-revision must be given together")
    return args


def main() -> int:
    args = parse_args()
    hex_names, z_names = read_exiftool_tables(args.exiftool_nikon_pm)

    sources = [f"ExifTool {args.exiftool_version} lib/Image/ExifTool/Nikon.pm"]
    exiv2_only_ids: list[str] = []
    if args.exiv2_nikonmn_int is not None:
        for key, name in read_exiv2_z_table(args.exiv2_nikonmn_int).items():
            if key not in z_names:
                z_names[key] = name
                exiv2_only_ids.append(key)
        sources.append(f"Exiv2 {args.exiv2_revision} src/nikonmn_int.cpp nikonZMountLensId")

    payload = {
        "meta": {
            "sources": sources,
            "generated_by": "scripts/build_nikon_lens_id_map.py",
            "hex_entries": len(hex_names),
            "numeric_entries": len(z_names),
            "numeric_ids_from_exiv2_only": sorted(exiv2_only_ids, key=int),
        },
        "hex_id_map": dict(sorted(hex_names.items())),
        "numeric_id_map": dict(sorted(z_names.items(), key=lambda item: int(item[0]))),
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print(
        f"Wrote {args.output} with {len(hex_names)} F-mount IDs and {len(z_names)} Z-mount IDs "
        f"({len(exiv2_only_ids)} Z-mount IDs from Exiv2 only)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
