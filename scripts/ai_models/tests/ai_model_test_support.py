"""Shared builders for the AI model package tooling tests.

``write_cpp_fixture`` writes the feed that the C++ tests read
(``alcedo_studio/tests/resources/ai_model_packages/``). Regenerate it after a format change with
``python scripts/ai_models/tests/ai_model_test_support.py --write-cpp-fixture``.
"""

from __future__ import annotations

import base64
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile

SCRIPTS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = SCRIPTS_DIR.parents[1]
CPP_FIXTURE_DIR = REPO_ROOT / "alcedo_studio" / "tests" / "resources" / "ai_model_packages"
sys.path.insert(0, str(SCRIPTS_DIR))

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

import ai_model_packages  # noqa: E402

# KeyPair() of ai_model_package_service_test.cpp uses the same seed bytes.
TEST_SIGNING_SEED = bytes((index * 5 + 3) % 256 for index in range(32))
FIXTURE_SEQUENCE = 20261027120000


class TemporaryDirectory:
    """A temporary directory under build/tmp, as the repository requires."""

    def __enter__(self) -> Path:
        root = REPO_ROOT / "build" / "tmp" / "ai-model-tooling-tests"
        root.mkdir(parents=True, exist_ok=True)
        self.path = Path(tempfile.mkdtemp(dir=root))
        return self.path

    def __exit__(self, *_exc) -> None:
        shutil.rmtree(self.path, ignore_errors=True)


def test_public_key_base64() -> str:
    raw = Ed25519PrivateKey.from_private_bytes(TEST_SIGNING_SEED).public_key().public_bytes_raw()
    return base64.b64encode(raw).decode("ascii")


def sign_with_test_key(manifest_path: Path, signature_path: Path) -> None:
    signature = Ed25519PrivateKey.from_private_bytes(TEST_SIGNING_SEED).sign(
        manifest_path.read_bytes())
    signature_path.write_bytes(base64.b64encode(signature) + b"\n")


def _content(path: str, platform: str) -> bytes:
    """Small file content that differs per file and platform."""
    return f"{platform}:{path}\n".encode("utf-8") * 3


def _pin(path: str, data: bytes) -> dict:
    return {"path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def write_test_model_set(work: Path, revision: str = "r1") -> tuple[Path, dict[str, Path]]:
    """A copy of the real AI Mask definition with small model files, notice, and licenses.

    Keeps the package ID, version, model roles, model file names, and license names and paths of
    ``ai_mask_model_set_v1.json``. Returns the definition path and the source directory per
    platform.
    """
    definition = json.loads(ai_model_packages.DEFAULT_DEFINITION.read_text(encoding="utf-8"))
    definition["revision"] = revision
    work.mkdir(parents=True, exist_ok=True)
    # Bytes, not text: Windows text mode writes CRLF, and the fixture must not depend on the OS.
    (work / "NOTICE.txt").write_bytes(b"Test notice\n")
    definition["notice"] = "NOTICE.txt"
    for index, license in enumerate(definition["licenses"]):
        source = work / "license_sources" / f"{index}.txt"
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(f"{license['name']} test text\n".encode("utf-8"))
        license["source"] = source.relative_to(work).as_posix()
        license.pop("sha256", None)
        license.pop("end_before", None)
    sources: dict[str, Path] = {}
    for platform, pins in definition["platforms"].items():
        root = work / "sources" / platform
        sources[platform] = root
        for pin in pins:
            if "files" not in pin:
                data = _content(pin["path"], platform)
                (root / pin["path"]).parent.mkdir(parents=True, exist_ok=True)
                (root / pin["path"]).write_bytes(data)
                pin.update(_pin(pin["path"], data))
                continue
            files = []
            for item in pin["files"]:
                data = _content(f"{pin['path']}/{item['path']}", platform)
                target = root / pin["path"] / item["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                files.append(_pin(item["path"], data))
            pin["files"] = files
            pin["size"] = sum(item["size"] for item in files)
            pin["directory_sha256"] = ai_model_packages.directory_sha256(root / pin["path"])
    path = work / "definition.json"
    path.write_text(json.dumps(definition, indent=2) + "\n", encoding="utf-8")
    return path, sources


def build_test_feed(work: Path, output: Path, sequence: int = FIXTURE_SEQUENCE,
                    revision: str = "r1") -> dict:
    """Prepare the test model set for both platforms into ``output`` and sign it."""
    definition_path, sources = write_test_model_set(work / "model_set", revision)
    definition = ai_model_packages.load_definition(definition_path)
    artifacts = {platform: ai_model_packages.build_package(
        definition, platform, sources[platform], output, ai_model_packages.DEFAULT_PREFIX,
        ai_model_packages.DEFAULT_PUBLIC_BASE) for platform in ai_model_packages.PLATFORMS}
    manifest_path = output / "manifest.json"
    manifest_path.write_bytes(ai_model_packages.manifest_bytes(
        sequence, [ai_model_packages.feed_entry(definition, artifacts)]))
    sign_with_test_key(manifest_path, output / "manifest.json.sig")
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def write_cpp_fixture() -> None:
    with TemporaryDirectory() as work:
        output = work / "out"
        build_test_feed(work, output)
        CPP_FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
        for old in CPP_FIXTURE_DIR.iterdir():
            old.unlink()
        for name in ("manifest.json", "manifest.json.sig"):
            shutil.copyfile(output / name, CPP_FIXTURE_DIR / name)
        for archive in (output / "packages").iterdir():
            shutil.copyfile(archive, CPP_FIXTURE_DIR / archive.name)
        (CPP_FIXTURE_DIR / "test_public_key.txt").write_bytes(
            (test_public_key_base64() + "\n").encode("ascii"))


def without_archive_bytes(manifest: dict) -> dict:
    """The manifest without the archive size and SHA-256 (py7zr writes OS file attributes)."""
    result = copy.deepcopy(manifest)
    for package in result["packages"]:
        for artifact in package["artifacts"].values():
            artifact.pop("size")
            artifact.pop("sha256")
    return result


if __name__ == "__main__":
    if sys.argv[1:] != ["--write-cpp-fixture"]:
        raise SystemExit("usage: ai_model_test_support.py --write-cpp-fixture")
    write_cpp_fixture()
    print(f"Wrote {CPP_FIXTURE_DIR}")
