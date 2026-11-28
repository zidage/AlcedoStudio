#!/usr/bin/env python3
"""Check the model files of one model set, build one 7z per platform, and sign the AI model feed.

Every source file must match the size and SHA-256 that the model set definition pins
(``ai_mask_model_set_v1.json``, ``ai_denoise_model_set_v1.json``; Core ML ``.mlmodelc``
directories file by file and by the lab directory rule). Each package contains
``model_set.json``, ``package-inventory.json``, ``NOTICE.txt``, ``licenses/``, and the model files.

The feed lists every package. The script verifies the base feed (by default the live feed) with
the update public key, carries each base entry of another (id, model_set_version) unchanged, and
adds the entry that it builds; a base entry of the same (id, model_set_version) is replaced. The
exact base files go to ``<output>/base/``, where publish_ai_model_packages.py checks the carried
entries. ``--no-base-feed`` starts a feed with this package only.

Without --private-key the script stops after the local package checks and writes an unsigned
manifest for inspection. Nothing is uploaded; see publish_ai_model_packages.py.
"""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path
import subprocess
import sys

from ai_model_packages import (BASE_DIRECTORY_NAME, DEFAULT_DEFINITION, DEFAULT_PREFIX,
                               DEFAULT_PUBLIC_BASE, MANIFEST_FILE_NAME, PLATFORMS, REPO_ROOT,
                               SIGNATURE_FILE_NAME, AiModelPackageError, build_package,
                               feed_entry, load_base_feed, load_definition, manifest_bytes,
                               merge_feed_entries, verify_signature)


def default_signer() -> Path:
    executable = "alcedo_update_signer.exe" if sys.platform == "win32" else "alcedo_update_signer"
    for build in ("release", "debug", "macos-release", "macos-debug"):
        candidate = REPO_ROOT / "build" / build / "alcedo_studio" / "src" / executable
        if candidate.is_file():
            return candidate
    return REPO_ROOT / "build" / "release" / "alcedo_studio" / "src" / executable


def sign_manifest(signer: Path, private_key: Path, manifest_path: Path,
                  signature_path: Path) -> None:
    if not signer.is_file():
        raise SystemExit(f"alcedo_update_signer not found at {signer}; build it first.")
    subprocess.run([str(signer), "sign", "--private-key", str(private_key), "--manifest",
                    str(manifest_path), "--signature", str(signature_path)], check=True)


def parse_sources(parser: argparse.ArgumentParser, args: argparse.Namespace) -> dict[str, Path]:
    """The source directory of each platform from --source and the two platform options."""
    sources: dict[str, Path] = {}
    for item in args.source:
        platform, separator, directory = item.partition("=")
        if not separator or platform not in PLATFORMS or not directory:
            parser.error(f"--source must be PLATFORM=DIR with PLATFORM in {list(PLATFORMS)}")
        sources[platform] = Path(directory)
    for platform, directory in (("windows-x86_64", args.windows_source),
                                ("macos-arm64", args.macos_source)):
        if directory is not None:
            sources[platform] = directory
    return sources


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--definition", type=Path, default=DEFAULT_DEFINITION,
                        help="Model set definition with the pinned files.")
    parser.add_argument("--source", action="append", default=[], metavar="PLATFORM=DIR",
                        help="Directory with the model files of one platform (repeatable).")
    parser.add_argument("--windows-source", type=Path, default=None,
                        help="Same as --source windows-x86_64=DIR.")
    parser.add_argument("--macos-source", type=Path, default=None,
                        help="Same as --source macos-arm64=DIR.")
    parser.add_argument("--sequence", type=int, default=None,
                        help="Feed sequence; default is the UTC time as YYYYMMDDHHMMSS.")
    parser.add_argument("--public-base", default=DEFAULT_PUBLIC_BASE)
    parser.add_argument("--prefix", default=DEFAULT_PREFIX,
                        help="R2 key prefix of the AI model feed.")
    base = parser.add_mutually_exclusive_group()
    base.add_argument("--base-feed", default=None,
                      help="Directory with manifest.json and manifest.json.sig, or the HTTPS URL "
                           "of a manifest.json; default is the live feed.")
    base.add_argument("--no-base-feed", action="store_true",
                      help="Start a feed that lists only this package.")
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--private-key", type=Path, default=None)
    parser.add_argument("--public-key-file", type=Path,
                        default=REPO_ROOT / "alcedo_studio" / "src" / "config" /
                        "update_public_key.txt")
    parser.add_argument("--signer", type=Path, default=None)
    args = parser.parse_args(argv)

    sequence = args.sequence or int(dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d%H%M%S"))
    if sequence < 1:
        parser.error("sequence must be positive")
    prefix = args.prefix.strip("/")
    sources = parse_sources(parser, args)
    output_dir = args.output_dir or REPO_ROOT / "build" / "tmp" / "ai_models" / str(sequence)
    if output_dir.exists() and any(output_dir.iterdir()):
        parser.error(f"output directory is not empty: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)

    try:
        definition = load_definition(args.definition)
        missing = set(PLATFORMS) - set(definition["platforms"])
        if missing:
            raise AiModelPackageError(f"the definition pins no files for {sorted(missing)}")
        missing = set(PLATFORMS) - set(sources)
        if missing:
            raise AiModelPackageError(f"no source directory for {sorted(missing)}")

        base_entries: list[dict] = []
        if not args.no_base_feed:
            base_source = args.base_feed or f"{args.public_base.rstrip('/')}/{prefix}/manifest.json"
            public_key = args.public_key_file.read_text(encoding="utf-8").strip()
            base_feed, base_manifest, base_signature = load_base_feed(base_source, public_key)
            if sequence <= base_feed["sequence"]:
                raise AiModelPackageError(f"the sequence {sequence} is not above the sequence "
                                          f"{base_feed['sequence']} of the base feed")
            base_entries = base_feed["packages"]
            base_dir = output_dir / BASE_DIRECTORY_NAME
            base_dir.mkdir()
            (base_dir / MANIFEST_FILE_NAME).write_bytes(base_manifest)
            (base_dir / SIGNATURE_FILE_NAME).write_bytes(base_signature)
            print(f"Base feed {base_source}: sequence {base_feed['sequence']}, "
                  f"{len(base_entries)} entries (signature verified)")

        artifacts = {}
        for platform in PLATFORMS:
            artifact = build_package(definition, platform, sources[platform], output_dir,
                                     prefix, args.public_base)
            artifacts[platform] = artifact
            print(f"Validated {definition['id']} v{definition['model_set_version']} "
                  f"{definition['revision']} {platform}: {artifact['file_count']} files, "
                  f"{artifact['unpacked_bytes']} bytes, archive {artifact['size']} bytes")
        entries = merge_feed_entries(base_entries, [feed_entry(definition, artifacts)])
    except (AiModelPackageError, OSError) as error:
        print(f"FAILED: {error}")
        print("Publication stopped; the previous live feed is unchanged.")
        return 1

    for entry in entries[:-1]:
        print(f"Carried {entry['id']} v{entry['model_set_version']} {entry['revision']} "
              "from the base feed")
    manifest_path = output_dir / MANIFEST_FILE_NAME
    manifest_path.write_bytes(manifest_bytes(sequence, entries))
    print(f"Manifest: {manifest_path}")
    if args.private_key is None:
        print("No --private-key given: packages validated locally; the manifest is unsigned.")
        return 0

    signature_path = output_dir / SIGNATURE_FILE_NAME
    sign_manifest(args.signer or default_signer(), args.private_key, manifest_path,
                  signature_path)
    verify_signature(manifest_path, signature_path,
                     args.public_key_file.read_text(encoding="utf-8").strip())
    print(f"Signature: {signature_path} (verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
