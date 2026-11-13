"""Shared builders for the LUT tooling tests."""

from __future__ import annotations

import copy
from pathlib import Path
import shutil
import sys
import tempfile

SCRIPTS_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = SCRIPTS_DIR.parents[1]
FIXTURE_DIR = REPO_ROOT / "alcedo_studio" / "tests" / "resources" / "lut_metadata"
sys.path.insert(0, str(SCRIPTS_DIR))

NUMERIC_TABLE = (
    b"LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n0.5 0.25 1\n"
)

_FILM_METADATA = {
    "schema": 1,
    "id": "spectral_film_lut:kodak_vision3_250d_5207:kodak_vision_2383",
    "origin": "alcedo",
    "category": "film_simulation",
    "source": {"id": "spectral_film_lut", "name": "Spectral Film LUT"},
    "film": {"id": "kodak_vision3_250d_5207", "name": "Vision3 250D 5207", "brand": "Kodak"},
    "print": {"id": "kodak_vision_2383", "name": "Vision 2383", "brand": "Kodak",
              "kind": "film"},
    "input_space": "ACEScc",
    "output_space": "ACEScc",
}


def film_metadata(**changes) -> dict:
    metadata = copy.deepcopy(_FILM_METADATA)
    metadata.update(changes)
    return metadata


def plain_cube(title: str = "t") -> bytes:
    return b"# Attribution: generator\n" + f'TITLE "{title}"\n'.encode() + NUMERIC_TABLE


class TemporaryDirectory:
    """A temporary directory under build/tmp, as the repository requires."""

    def __enter__(self) -> Path:
        root = REPO_ROOT / "build" / "tmp" / "lut-tooling-tests"
        root.mkdir(parents=True, exist_ok=True)
        self.path = Path(tempfile.mkdtemp(dir=root))
        return self.path

    def __exit__(self, *_exc) -> None:
        shutil.rmtree(self.path, ignore_errors=True)
