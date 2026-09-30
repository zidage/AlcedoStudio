#!/usr/bin/env python3
"""Plan or upload a prepared, signed LUT package feed to R2.

Without --upload this lists every object key and uploads nothing; R2 credentials
are not read. With --upload the order is: package archives (immutable), the
archived signature and manifest (immutable), then the live signature and the
live manifest last. Any failure stops publication and leaves the live feed as it
was.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from lut_package_archive import verify_package_archive
from prepare_lut_packages import DEFAULT_PREFIX, REPO_ROOT, verify_signature

sys.path.insert(0, str(REPO_ROOT / "scripts" / "update"))
from publish_update import load_env_file, publish_object, require_env  # noqa: E402

IMMUTABLE = "public, max-age=31536000, immutable"
MUTABLE = "public, max-age=300, must-revalidate"


def planned_uploads(output_dir: Path, manifest: dict, prefix: str) -> list[dict]:
    """Return every upload in publication order with its key and cache policy."""
    uploads: list[dict] = []
    for package in manifest["packages"]:
        url = package["artifact"]["url"]
        _, separator, key = url.partition(f"/{prefix}/")
        if not separator:
            raise SystemExit(f"archive URL is not under the {prefix} prefix: {url}")
        key = f"{prefix}/{key}"
        uploads.append({
            "source": output_dir / "packages" / Path(key).name,
            "key": key,
            "content_type": "application/x-7z-compressed",
            "disposition": f'attachment; filename="{Path(key).name}"',
            "immutable": True,
            "role": f"package {package['id']}",
        })
    archived = f"{prefix}/manifests/{manifest['sequence']}"
    for name, content_type, immutable, key_prefix, role in (
            ("manifest.json.sig", "text/plain; charset=utf-8", True, archived, "archived signature"),
            ("manifest.json", "application/json; charset=utf-8", True, archived, "archived manifest"),
            ("manifest.json.sig", "text/plain; charset=utf-8", False, prefix, "live signature"),
            ("manifest.json", "application/json; charset=utf-8", False, prefix, "live manifest")):
        uploads.append({
            "source": output_dir / name,
            "key": f"{key_prefix}/{name}",
            "content_type": content_type,
            "disposition": f'inline; filename="{name}"',
            "immutable": immutable,
            "role": role,
        })
    return uploads


def verify_prepared_output(output_dir: Path, public_key_file: Path) -> dict:
    """Check the signature over the exact manifest bytes and every package archive."""
    manifest_path = output_dir / "manifest.json"
    signature_path = output_dir / "manifest.json.sig"
    if not signature_path.is_file():
        raise SystemExit("manifest.json.sig is missing; an unsigned feed is never published")
    verify_signature(manifest_path, signature_path,
                     public_key_file.read_text(encoding="utf-8").strip())
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for package in manifest["packages"]:
        archive = output_dir / "packages" / Path(package["artifact"]["url"]).name
        verify_package_archive(archive, package, output_dir / "verify")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Directory written by prepare_lut_packages.py")
    parser.add_argument("--prefix", default=DEFAULT_PREFIX)
    parser.add_argument("--public-key-file", type=Path,
                        default=REPO_ROOT / "alcedo_studio" / "src" / "config" /
                        "update_public_key.txt")
    parser.add_argument("--env-file", type=Path,
                        default=REPO_ROOT / "rust" / "puerh_mind" / ".env.test")
    parser.add_argument("--upload", action="store_true",
                        help="Upload for real. Without it the script only prints the plan.")
    args = parser.parse_args(argv)

    prefix = args.prefix.strip("/")
    manifest = verify_prepared_output(args.output_dir, args.public_key_file)
    uploads = planned_uploads(args.output_dir, manifest, prefix)
    print(f"Validated LUT feed sequence {manifest['sequence']}. Publication order:")
    for item in uploads:
        policy = "immutable" if item["immutable"] else "live"
        print(f"  {item['key']}  [{item['role']}, {policy}]")
    if not args.upload:
        print("Plan only. R2 credentials were not read. Nothing was published.")
        return 0

    load_env_file(args.env_file)
    account_id = require_env("R2_ACCOUNT_ID")
    bucket = require_env("R2_BUCKET")
    os.environ["AWS_ACCESS_KEY_ID"] = require_env("R2_ACCESS_KEY_ID")
    os.environ["AWS_SECRET_ACCESS_KEY"] = require_env("R2_SECRET_ACCESS_KEY")
    for key, value in (("AWS_REGION", "auto"), ("AWS_DEFAULT_REGION", "auto"),
                       ("AWS_EC2_METADATA_DISABLED", "true"), ("AWS_RETRY_MODE", "standard"),
                       ("AWS_MAX_ATTEMPTS", "10")):
        os.environ.setdefault(key, value)
    endpoint = f"https://{account_id}.r2.cloudflarestorage.com"
    for item in uploads:
        publish_object(endpoint, bucket, item["source"], item["key"], item["content_type"],
                       item["disposition"], IMMUTABLE if item["immutable"] else MUTABLE,
                       item["immutable"])
    print(f"Published LUT feed sequence {manifest['sequence']}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
