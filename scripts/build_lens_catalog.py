#!/usr/bin/env python3
"""Build brand -> lens model catalog from lens calibration XML files."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
import xml.etree.ElementTree as ET


def normalize_text(text: str | None) -> str:
    if text is None:
        return ""
    return " ".join(text.split()).strip()


def pick_preferred_text(parent: ET.Element, tag: str) -> str:
    nodes = parent.findall(tag)
    if not nodes:
        return ""

    for node in nodes:
        if "lang" not in node.attrib:
            value = normalize_text(node.text)
            if value:
                return value

    for node in nodes:
        value = normalize_text(node.text)
        if value:
            return value

    return ""


def iter_lens_entries(xml_path: Path) -> list[tuple[str, str]]:
    try:
        tree = ET.parse(xml_path)
    except ET.ParseError:
        return []

    root = tree.getroot()
    entries: list[tuple[str, str]] = []

    for lens_node in root.findall("lens"):
        maker = pick_preferred_text(lens_node, "maker")
        model = pick_preferred_text(lens_node, "model")
        if not maker or not model:
            continue
        entries.append((maker, model))

    return entries


def build_catalog(source_dir: Path) -> dict:
    brands_to_models: dict[str, set[str]] = defaultdict(set)
    source_files = 0
    lens_entries = 0

    for xml_path in sorted(source_dir.glob("*.xml")):
        source_files += 1
        for maker, model in iter_lens_entries(xml_path):
            brands_to_models[maker].add(model)
            lens_entries += 1

    sorted_catalog: dict[str, list[str]] = {}
    for maker in sorted(brands_to_models.keys(), key=lambda s: s.casefold()):
        models = sorted(brands_to_models[maker], key=lambda s: s.casefold())
        if models:
            sorted_catalog[maker] = models

    return {
        "generated_by": "scripts/build_lens_catalog.py",
        "generated_at_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source_dir": source_dir.as_posix(),
        "source_files": source_files,
        "lens_entries": lens_entries,
        "brands": sorted_catalog,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build lens catalog JSON from lens calibration XML files.")
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("alcedo_studio/src/config/lens_calib"),
        help="Directory containing lens calibration XML files.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("alcedo_studio/src/config/lens_calib/lens_catalog.json"),
        help="Output JSON file path.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_dir: Path = args.source
    output_path: Path = args.output

    if not source_dir.exists() or not source_dir.is_dir():
        raise SystemExit(f"Source directory does not exist: {source_dir}")

    catalog = build_catalog(source_dir)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(catalog, f, indent=2, ensure_ascii=True)
        f.write("\n")

    brand_count = len(catalog["brands"])
    model_count = sum(len(models) for models in catalog["brands"].values())
    print(
        f"Wrote {output_path} with {brand_count} brands and {model_count} unique models "
        f"from {catalog['source_files']} XML files."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
