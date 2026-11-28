#!/usr/bin/env python3
"""Plan or upload a prepared, signed AI model feed to R2.

Without --upload this checks the prepared output, lists every object key, and uploads nothing;
R2 credentials are not read. With --upload the order is: the package archives (immutable), the
archived signature and manifest (immutable), then the live signature and the live manifest last.
Any failure stops publication and leaves the live feed as it was.

An entry that prepare_ai_model_packages.py carried from the base feed (``<output>/base/``) has no
archive in the output: it must equal an entry of the verified base feed, its archives are not
uploaded again, and with --upload each of them must be on the server with its signed size before
the first upload.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from ai_model_packages import (BASE_DIRECTORY_NAME, DEFAULT_PREFIX, REPO_ROOT,
                               AiModelPackageError, entry_key, fetch_url, load_base_feed,
                               verify_package_archive, verify_signature)

sys.path.insert(0, str(REPO_ROOT / "scripts" / "update"))
from publish_update import load_env_file, publish_object, require_env  # noqa: E402

IMMUTABLE = "public, max-age=31536000, immutable"
MUTABLE = "public, max-age=300, must-revalidate"


def archive_object_key(url: str, prefix: str) -> str:
    _, separator, key = url.partition(f"/{prefix}/packages/")
    if not separator:
        raise AiModelPackageError(f"archive URL is not under {prefix}/packages/: {url}")
    return f"{prefix}/packages/{key}"


def planned_uploads(output_dir: Path, manifest: dict, prefix: str,
                    carried: frozenset = frozenset()) -> list[dict]:
    """Return every upload in publication order with its key and cache policy.

    ``carried`` holds the (id, model_set_version) of the entries whose archives are already on
    the server; they are not uploaded.
    """
    uploads: list[dict] = []
    for package in manifest["packages"]:
        if entry_key(package) in carried:
            continue
        for platform, artifact in sorted(package["artifacts"].items()):
            key = archive_object_key(artifact["url"], prefix)
            uploads.append({
                "source": output_dir / "packages" / Path(key).name,
                "key": key,
                "content_type": "application/x-7z-compressed",
                "disposition": f'attachment; filename="{Path(key).name}"',
                "immutable": True,
                "role": f"package {package['id']} v{package['model_set_version']} "
                        f"{package['revision']} {platform}",
            })
    archived = f"{prefix}/manifests/{manifest['sequence']}"
    signature_type = "text/plain; charset=utf-8"
    manifest_type = "application/json; charset=utf-8"
    for name, content_type, immutable, key_prefix, role in (
            ("manifest.json.sig", signature_type, True, archived, "archived signature"),
            ("manifest.json", manifest_type, True, archived, "archived manifest"),
            ("manifest.json.sig", signature_type, False, prefix, "live signature"),
            ("manifest.json", manifest_type, False, prefix, "live manifest")):
        uploads.append({
            "source": output_dir / name,
            "key": f"{key_prefix}/{name}",
            "content_type": content_type,
            "disposition": f'inline; filename="{name}"',
            "immutable": immutable,
            "role": role,
        })
    return uploads


def verify_prepared_output(output_dir: Path, public_key_file: Path) -> tuple[dict, frozenset]:
    """Check the signature over the exact manifest bytes, every new archive, and every carried
    entry against the verified base feed. Returns the feed and the keys of the carried entries.
    """
    manifest_path = output_dir / "manifest.json"
    signature_path = output_dir / "manifest.json.sig"
    if not signature_path.is_file():
        raise SystemExit("manifest.json.sig is missing; an unsigned feed is never published")
    public_key = public_key_file.read_text(encoding="utf-8").strip()
    verify_signature(manifest_path, signature_path, public_key)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    base_entries: list[dict] = []
    base_dir = output_dir / BASE_DIRECTORY_NAME
    if base_dir.is_dir():
        base_feed, _, _ = load_base_feed(str(base_dir), public_key)
        if manifest["sequence"] <= base_feed["sequence"]:
            raise AiModelPackageError("the feed sequence is not above the base feed sequence")
        base_entries = base_feed["packages"]
    carried = set()
    for package in manifest["packages"]:
        if package in base_entries:
            carried.add(entry_key(package))
            continue
        for platform, artifact in package["artifacts"].items():
            archive = output_dir / "packages" / Path(artifact["url"]).name
            if not archive.is_file():
                raise AiModelPackageError(
                    f"{package['id']} v{package['model_set_version']} {platform} has no archive "
                    "in the output and is not an entry of the base feed")
            verify_package_archive(archive, package, platform, output_dir / "verify")
    return manifest, frozenset(carried)


def check_carried_archives(manifest: dict, carried: frozenset) -> None:
    """Each archive of a carried entry answers a HEAD request with 200 and its signed size."""
    for package in manifest["packages"]:
        if entry_key(package) not in carried:
            continue
        for artifact in package["artifacts"].values():
            status, headers, _ = fetch_url(artifact["url"], method="HEAD")
            length = {key.lower(): value for key, value in headers.items()}.get("content-length")
            if status != 200 or length != str(artifact["size"]):
                raise AiModelPackageError(
                    f"the carried archive {artifact['url']} returned HTTP {status} with "
                    f"{length} bytes, signed {artifact['size']}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Directory written by prepare_ai_model_packages.py")
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
    manifest, carried = verify_prepared_output(args.output_dir, args.public_key_file)
    uploads = planned_uploads(args.output_dir, manifest, prefix, carried)
    print(f"Validated AI model feed sequence {manifest['sequence']}.")
    for package in manifest["packages"]:
        if entry_key(package) in carried:
            print(f"  Carried from the base feed, not uploaded: {package['id']} "
                  f"v{package['model_set_version']} {package['revision']}")
    print("Publication order:")
    for item in uploads:
        policy = "immutable" if item["immutable"] else "live"
        print(f"  {item['key']}  [{item['role']}, {policy}]")
    if not args.upload:
        print("Plan only. R2 credentials were not read. Nothing was published.")
        return 0

    check_carried_archives(manifest, carried)
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
    print(f"Published AI model feed sequence {manifest['sequence']}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
