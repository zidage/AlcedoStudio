"""Tests for the AI model package preparation, archive verification, and publication plan."""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import re
import subprocess
import unittest

import py7zr

from ai_model_test_support import (CPP_FIXTURE_DIR, REPO_ROOT, TemporaryDirectory,
                                   build_test_feed, test_public_key_base64, without_archive_bytes,
                                   write_test_model_set)

import ai_model_packages
from ai_model_packages import AiModelPackageError, verify_package_archive, verify_signature
import prepare_ai_model_packages
import publish_ai_model_packages

MASTER_PLAN = (REPO_ROOT / "docs" / "roadmap" / "alcedo_studio" / "edit" /
               "ai_segmentation_mask_master_plan.md")


def run_quietly(function, argv: list[str]) -> tuple[int, str]:
    with contextlib.redirect_stdout(io.StringIO()) as printed:
        code = function(argv)
    return code, printed.getvalue()


def find_signer() -> Path | None:
    signer = prepare_ai_model_packages.default_signer()
    return signer if signer.is_file() else None


def archive_path(directory: Path, manifest: dict, platform: str) -> Path:
    """The archive of ``platform`` in ``directory`` (``packages/`` of a prepared output)."""
    url = manifest["packages"][0]["artifacts"][platform]["url"]
    return directory / Path(url).name


def extracted_files(archive: Path, work: Path) -> dict[str, bytes]:
    target = work / archive.stem
    with py7zr.SevenZipFile(archive, "r") as source:
        source.extractall(path=target)
    return {path.relative_to(target).as_posix(): path.read_bytes()
            for path in sorted(target.rglob("*")) if path.is_file()}


def repack(files: dict[str, bytes], work: Path, name: str) -> Path:
    root = work / f"{name}-content"
    for relative, data in files.items():
        (root / relative).parent.mkdir(parents=True, exist_ok=True)
        (root / relative).write_bytes(data)
    archive = work / f"{name}.7z"
    with py7zr.SevenZipFile(archive, "w") as target:
        for relative in sorted(files):
            target.write(root / relative, relative)
    return archive


def section_8_2_1() -> str:
    text = MASTER_PLAN.read_text(encoding="utf-8")
    return text[text.index("#### 8.2.1 Model set"):text.index("#### 8.2.2")]


class AiModelPackagePreparationTest(unittest.TestCase):
    def prepare(self, work: Path, *extra: str, sources: dict | None = None,
                definition: Path | None = None) -> tuple[int, Path, str]:
        if definition is None:
            definition, sources = write_test_model_set(work / "model_set")
        output = work / "out"
        code, printed = run_quietly(prepare_ai_model_packages.main, [
            "--definition", str(definition),
            "--windows-source", str(sources["windows-x86_64"]),
            "--macos-source", str(sources["macos-arm64"]),
            "--sequence", "42", "--output-dir", str(output), *extra])
        return code, output, printed

    def test_PreparedArchivesVerifyFromLocalFiles(self) -> None:
        with TemporaryDirectory() as work:
            code, output, printed = self.prepare(work)
            self.assertEqual(code, 0, printed)
            self.assertFalse((output / "manifest.json.sig").exists())
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual((manifest["schema"], manifest["kind"], manifest["sequence"]),
                             (1, "alcedo-ai-model-packages", 42))
            [entry] = manifest["packages"]
            self.assertEqual((entry["id"], entry["name"], entry["model_set_version"],
                              entry["revision"]), ("alcedo-ai-mask", "AI Mask", 1, "r1"))
            self.assertEqual([item["name"] for item in entry["licenses"]],
                             ["Apache-2.0", "MIT", "DINOv3 License", "GPL-3.0"])
            self.assertEqual(sorted(entry["artifacts"]), ["macos-arm64", "windows-x86_64"])
            for platform, artifact in entry["artifacts"].items():
                self.assertEqual(
                    artifact["url"],
                    "https://static.aoraw.org/ai-models/v1/packages/alcedo-ai-mask/v1/r1/"
                    f"alcedo-ai-mask-v1-r1-{platform}.7z")
                archive = archive_path(output / "packages", manifest, platform)
                inventory = verify_package_archive(archive, entry, platform, work / "verify")
                paths = [item["path"] for item in inventory["files"]]
                self.assertEqual(paths, sorted(paths, key=lambda value: value.encode("utf-8")))
                for expected in ("model_set.json", "NOTICE.txt", "people_sky_head.json",
                                 "licenses/SAM2-Apache-2.0.txt", "licenses/S3OD-MIT.txt",
                                 "licenses/DINOv3-License.md", "licenses/GPL-3.0.txt"):
                    self.assertIn(expected, paths)
                self.assertNotIn("package-inventory.json", paths)
                self.assertEqual(artifact["file_count"], len(paths))
                files = extracted_files(archive, work)
                model_set = json.loads(files["model_set.json"])
                self.assertEqual(model_set["platform"], platform)
                self.assertEqual((model_set["kind"], model_set["model_set_version"],
                                  model_set["revision"]), ("alcedo-ai-model-set", 1, "r1"))
                roles = {model["role"]: model["path"] for model in model_set["models"]}
                if platform == "windows-x86_64":
                    self.assertEqual(roles["subject_masks"], "subject_masks.onnx")
                    self.assertIn("point_encoder.onnx", paths)
                else:
                    # Core ML directories are directories in the archive; every file is listed.
                    self.assertEqual(roles["subject_masks"], "s3od.mlmodelc")
                    self.assertIn("s3od.mlmodelc/weights/weight.bin", paths)
                    self.assertIn("point_encoder.mlmodelc/analytics/coremldata.bin", paths)
            self.assertFalse((output / "staging").exists())

    def test_ChangedSourceByteStopsPreparationWithoutManifest(self) -> None:
        cases = {
            "onnx byte": ("windows-x86_64", "point_encoder.onnx"),
            "Core ML weight byte": ("macos-arm64", "s3od.mlmodelc/weights/weight.bin"),
            "thresholds byte": ("macos-arm64", "people_sky_head.json"),
        }
        for name, (platform, relative) in cases.items():
            with self.subTest(case=name), TemporaryDirectory() as work:
                definition, sources = write_test_model_set(work / "model_set")
                path = sources[platform] / relative
                data = bytearray(path.read_bytes())
                data[0] ^= 0x01
                path.write_bytes(bytes(data))
                code, output, printed = self.prepare(work, sources=sources,
                                                     definition=definition)
                self.assertEqual(code, 1)
                self.assertIn("differs from its pinned", printed)
                self.assertFalse((output / "manifest.json").exists())

    def test_ExtraOrMissingCoreMlFileStopsPreparation(self) -> None:
        for change in ("extra", "missing"):
            with self.subTest(change=change), TemporaryDirectory() as work:
                definition, sources = write_test_model_set(work / "model_set")
                directory = sources["macos-arm64"] / "sky_head.mlmodelc"
                if change == "extra":
                    (directory / "metadata.json").write_text("{}", encoding="utf-8")
                else:
                    (directory / "coremldata.bin").unlink()
                code, output, printed = self.prepare(work, sources=sources,
                                                     definition=definition)
                self.assertEqual(code, 1)
                self.assertIn("other files than its pins", printed)
                self.assertFalse((output / "manifest.json").exists())

    def test_ChangedArchiveByteFailsVerification(self) -> None:
        with TemporaryDirectory() as work:
            manifest = build_test_feed(work, work / "out")
            entry = manifest["packages"][0]
            archive = archive_path(work / "out" / "packages", manifest, "windows-x86_64")
            files = extracted_files(archive, work)

            # One changed byte of the archive fails its signed size and SHA-256.
            data = bytearray(archive.read_bytes())
            data[len(data) // 2] ^= 0x01
            changed = work / "changed.7z"
            changed.write_bytes(bytes(data))
            with self.assertRaisesRegex(AiModelPackageError, "feed size and SHA-256"):
                verify_package_archive(changed, entry, "windows-x86_64", work / "verify")

            # A changed model byte in a valid archive fails the inventory check, and a changed
            # inventory fails the signed inventory digest.
            descriptor = json.loads(json.dumps(entry))
            for name, target in (("model", "sky_head.onnx"),
                                 ("inventory", "package-inventory.json")):
                forged_files = dict(files)
                forged_files[target] = files[target].replace(b"windows", b"wINdows", 1)
                forged = repack(forged_files, work, name)
                sha256, size = ai_model_packages.hash_file(forged)
                descriptor["artifacts"]["windows-x86_64"] |= {"sha256": sha256, "size": size}
                with self.subTest(changed=name), self.assertRaises(AiModelPackageError):
                    verify_package_archive(forged, descriptor, "windows-x86_64",
                                           work / "verify")

    def test_SameInputGivesSameArchiveBytes(self) -> None:
        with TemporaryDirectory() as work:
            first = build_test_feed(work / "a", work / "a" / "out")
            second = build_test_feed(work / "b", work / "b" / "out")
            self.assertEqual(first, second)

    @unittest.skipIf(find_signer() is None, "alcedo_update_signer is not built")
    def test_SignerSignsFeedAndPublicationPlanListsLiveManifestLast(self) -> None:
        with TemporaryDirectory() as work:
            signer = find_signer()
            private_key = work / "key.seed"
            public_key = work / "key.pub"
            subprocess.run([str(signer), "generate", "--private-key", str(private_key),
                            "--public-key", str(public_key)], check=True, capture_output=True)
            code, output, printed = self.prepare(work, "--private-key", str(private_key),
                                                 "--public-key-file", str(public_key))
            self.assertEqual(code, 0, printed)
            self.assertIn("(verified)", printed)

            publish_args = ["--output-dir", str(output), "--public-key-file", str(public_key)]
            code, printed = run_quietly(publish_ai_model_packages.main, publish_args)
            self.assertEqual(code, 0)
            self.assertIn("Nothing was published", printed)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            keys = [item["key"] for item in
                    publish_ai_model_packages.planned_uploads(output, manifest, "ai-models/v1")]
            self.assertEqual(keys, [
                "ai-models/v1/packages/alcedo-ai-mask/v1/r1/alcedo-ai-mask-v1-r1-macos-arm64.7z",
                "ai-models/v1/packages/alcedo-ai-mask/v1/r1/"
                "alcedo-ai-mask-v1-r1-windows-x86_64.7z",
                "ai-models/v1/manifests/42/manifest.json.sig",
                "ai-models/v1/manifests/42/manifest.json",
                "ai-models/v1/manifest.json.sig",
                "ai-models/v1/manifest.json",
            ])
            uploads = publish_ai_model_packages.planned_uploads(output, manifest, "ai-models/v1")
            self.assertEqual([item["immutable"] for item in uploads],
                             [True, True, True, True, False, False])
            self.assertTrue(all(item["source"].is_file() for item in uploads))

            # A mismatched signature and manifest pair is rejected, never published unsigned.
            manifest_path = output / "manifest.json"
            manifest_path.write_bytes(
                manifest_path.read_bytes().replace(b'"sequence": 42', b'"sequence": 43'))
            with self.assertRaisesRegex(AiModelPackageError, "signature"):
                run_quietly(publish_ai_model_packages.main, publish_args)
            (output / "manifest.json.sig").unlink()
            with self.assertRaises(SystemExit):
                run_quietly(publish_ai_model_packages.main, publish_args)

    def test_PublicationRejectsChangedArchive(self) -> None:
        with TemporaryDirectory() as work:
            output = work / "out"
            build_test_feed(work, output)
            key_file = work / "key.txt"
            key_file.write_text(test_public_key_base64(), encoding="utf-8")
            publish_args = ["--output-dir", str(output), "--public-key-file", str(key_file)]
            self.assertEqual(run_quietly(publish_ai_model_packages.main, publish_args)[0], 0)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            archive = archive_path(output / "packages", manifest, "macos-arm64")
            archive.write_bytes(archive.read_bytes() + b"\0")
            with self.assertRaisesRegex(AiModelPackageError, "feed size and SHA-256"):
                run_quietly(publish_ai_model_packages.main, publish_args)


class AiModelPackageCppFixtureTest(unittest.TestCase):
    def test_CppFixtureIsTheCurrentPythonPreparedFeed(self) -> None:
        # AiModelPackageManifestTest.PythonPreparedFeedVerifiesInCpp and
        # AiModelPackageServiceTest.PythonPreparedPackageInstallsOnEachPlatform read these files.
        committed = json.loads((CPP_FIXTURE_DIR / "manifest.json").read_text(encoding="utf-8"))
        verify_signature(CPP_FIXTURE_DIR / "manifest.json", CPP_FIXTURE_DIR / "manifest.json.sig",
                         (CPP_FIXTURE_DIR / "test_public_key.txt").read_text().strip())
        with TemporaryDirectory() as work:
            for platform in ai_model_packages.PLATFORMS:
                verify_package_archive(archive_path(CPP_FIXTURE_DIR, committed, platform),
                                       committed["packages"][0], platform, work / "verify")
            output = work / "out"
            current = build_test_feed(work, output)
            self.assertEqual(without_archive_bytes(current), without_archive_bytes(committed))
            for platform in ai_model_packages.PLATFORMS:
                self.assertEqual(
                    extracted_files(archive_path(output / "packages", current, platform),
                                    work / "new"),
                    extracted_files(archive_path(CPP_FIXTURE_DIR, committed, platform),
                                    work / "old"))


class AiMaskModelSetDefinitionTest(unittest.TestCase):
    def test_DefinitionPinsTheAcceptedModelFilesAndLicenseTexts(self) -> None:
        definition = ai_model_packages.load_definition(ai_model_packages.DEFAULT_DEFINITION)
        plan = section_8_2_1()
        # Windows files: the size and SHA-256 of master plan section 8.2.1.
        for pin in definition["platforms"]["windows-x86_64"]:
            with self.subTest(file=pin["path"]):
                self.assertIn(pin["sha256"], plan)
                if pin["path"].endswith(".onnx"):
                    self.assertIn(f"`{pin['path']}`", plan)
                    self.assertIn(f"{pin['size']:,} bytes", plan)
        # Core ML directories: the byte total of section 8.2.1; every file is pinned.
        for pin in definition["platforms"]["macos-arm64"]:
            with self.subTest(file=pin["path"]):
                if "files" not in pin:
                    self.assertIn(pin["sha256"], plan)
                    continue
                self.assertIn(f"{pin['size']:,} bytes", plan)
                self.assertEqual(pin["size"], sum(item["size"] for item in pin["files"]))
                self.assertEqual(sorted(item["path"] for item in pin["files"]),
                                 ["analytics/coremldata.bin", "coremldata.bin", "model.mil",
                                  "weights/weight.bin"])
                self.assertIn(pin["directory_sha256"], plan)
        # The sky head is the copy that section 8.2.1 accepted, file by file.
        sky = next(pin for pin in definition["platforms"]["macos-arm64"]
                   if pin["path"] == "sky_head.mlmodelc")
        for item in sky["files"]:
            if item["path"] in ("model.mil", "weights/weight.bin"):
                self.assertIn(item["sha256"], plan)

        texts = {license["name"]: ai_model_packages.license_text(license)
                 for license in definition["licenses"]}
        self.assertTrue(texts["Apache-2.0"].lstrip().startswith(b"Apache License"))
        self.assertIn(b"Copyright (c) 2024 S3OD Team", texts["MIT"])
        self.assertTrue(texts["DINOv3 License"].startswith(b"# DINOv3 License"))
        self.assertTrue(texts["GPL-3.0"].lstrip().startswith(b"GNU GENERAL PUBLIC LICENSE"))
        self.assertNotIn(b"NVIDIA", texts["GPL-3.0"])
        self.assertTrue(
            texts["GPL-3.0"].endswith(b"<http://www.gnu.org/philosophy/why-not-lgpl.html>.\n"))

    def test_NoticeNamesEveryModelLicenseAndTheStatement(self) -> None:
        definition = ai_model_packages.load_definition(ai_model_packages.DEFAULT_DEFINITION)
        notice = definition["notice"].read_text(encoding="utf-8")
        for license in definition["licenses"]:
            self.assertIn(license["file"], notice)
        for model in definition["models"]:
            for path in model["files"].values():
                self.assertIn(path, notice)
        for text in ("MaSS13K", "FLUX.1 Krea [dev]", "DIS5K", "DUTS",
                     "Before you download", "下载前请阅读", "Accept and Download"):
            self.assertIn(text, notice)
        # Decision A7: the statement does not forbid commercial use.
        self.assertIsNone(re.search(r"(?i)do not use .* commercial", notice))


if __name__ == "__main__":
    unittest.main()
