#!/usr/bin/env python3
"""Read, validate, and write the ALCEDO_LUT CUBE metadata comment (schema 1).

The rules mirror alcedo_studio/src/utils/lut/lut_metadata.cpp and
docs/lut-package-system.md. Validation returns explicit error strings; it never
turns malformed metadata into an unannotated LUT.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import re

MARKER = "ALCEDO_LUT"
SCHEMA_VERSION = 1
HEADER_LIMIT_BYTES = 64 * 1024
METADATA_LINE_LIMIT_BYTES = 16 * 1024
ID_LIMIT_BYTES = 160
SLUG_LIMIT_BYTES = 96
NAME_LIMIT_BYTES = 128
SPACE_LIMIT_BYTES = 64
DESCRIPTION_LIMIT_BYTES = 1024
DISPLAY_REQUIREMENT_LIMIT_BYTES = 512
ALIAS_LIMIT_COUNT = 16
ALIAS_LIMIT_BYTES = 128

_ID_PATTERN = re.compile(r"^[a-z0-9]+(?:[._:-][a-z0-9]+)*$")
_SLUG_PATTERN = re.compile(r"^[a-z0-9]+(?:[._-][a-z0-9]+)*$")
_NUMERIC_START = set("0123456789+-.")


class LutMetadataError(ValueError):
    """Raised when a metadata comment or CUBE header violates schema 1."""


@dataclass
class CubeHeader:
    """Header facts read before the first numeric row."""

    metadata: dict | None = None
    title: str = ""
    lut_3d_size: int = 0
    lut_1d_size: int = 0
    header_bytes: int = 0
    extra: dict = field(default_factory=dict)


def _utf8_len(text: str) -> int:
    return len(text.encode("utf-8"))


def _has_control(text: str) -> bool:
    return any(ord(character) < 0x20 or ord(character) == 0x7F for character in text)


def _check_text(value: object, label: str, limit: int, errors: list[str]) -> None:
    if not isinstance(value, str) or not value:
        errors.append(f"{label} must be a non-empty string")
        return
    if _utf8_len(value) > limit:
        errors.append(f"{label} exceeds {limit} bytes")
    if _has_control(value):
        errors.append(f"{label} contains a control character")


def _check_slug(value: object, label: str, errors: list[str]) -> None:
    if not isinstance(value, str) or not _SLUG_PATTERN.match(value) or \
            _utf8_len(value) > SLUG_LIMIT_BYTES:
        errors.append(f"{label} must be a slug of at most {SLUG_LIMIT_BYTES} bytes")


def _check_object(metadata: dict, name: str, keys: tuple[str, ...], errors: list[str]) -> dict:
    value = metadata.get(name)
    if not isinstance(value, dict):
        errors.append(f"{name} must be an object")
        return {}
    for key in keys:
        if key == "id":
            _check_slug(value.get(key), f"{name}.id", errors)
        elif key != "kind":
            _check_text(value.get(key), f"{name}.{key}", NAME_LIMIT_BYTES, errors)
    return value


def validate_metadata(metadata: object) -> list[str]:
    """Return every schema-1 violation in ``metadata``; an empty list means valid."""
    if not isinstance(metadata, dict):
        return ["metadata must be a JSON object"]
    errors: list[str] = []
    schema = metadata.get("schema")
    if not isinstance(schema, int) or isinstance(schema, bool) or schema != SCHEMA_VERSION:
        return [f"unsupported schema: {schema!r}"]

    lut_id = metadata.get("id")
    if not isinstance(lut_id, str) or not _ID_PATTERN.match(lut_id) or \
            _utf8_len(lut_id) > ID_LIMIT_BYTES:
        errors.append(f"id must match the identifier rule and be at most {ID_LIMIT_BYTES} bytes")
    if metadata.get("origin") not in ("alcedo", "user"):
        errors.append("origin must be 'alcedo' or 'user'")
    category = metadata.get("category")
    if category not in ("general", "film_simulation"):
        errors.append("category must be 'general' or 'film_simulation'")

    if "source" in metadata or category == "film_simulation":
        _check_object(metadata, "source", ("id", "name"), errors)
    if category == "film_simulation":
        _check_object(metadata, "film", ("id", "name", "brand"), errors)
    elif category == "general":
        for name in ("film", "print"):
            if name in metadata:
                errors.append(f"{name} is not allowed on a general LUT")
    if "print" in metadata and category == "film_simulation":
        print_value = _check_object(metadata, "print", ("id", "name", "brand", "kind"), errors)
        if print_value and print_value.get("kind") not in ("film", "paper"):
            errors.append("print.kind must be 'film' or 'paper'")
    if "variant" in metadata:
        _check_slug(metadata["variant"], "variant", errors)

    for name in ("input_space", "output_space"):
        _check_text(metadata.get(name), name, SPACE_LIMIT_BYTES, errors)
    if "description" in metadata:
        _check_text(metadata["description"], "description", DESCRIPTION_LIMIT_BYTES, errors)
    if "display_requirement" in metadata:
        _check_text(metadata["display_requirement"], "display_requirement",
                    DISPLAY_REQUIREMENT_LIMIT_BYTES, errors)
    if "aliases" in metadata:
        aliases = metadata["aliases"]
        if not isinstance(aliases, list) or len(aliases) > ALIAS_LIMIT_COUNT:
            errors.append(f"aliases must be a list of at most {ALIAS_LIMIT_COUNT} strings")
        else:
            for index, alias in enumerate(aliases):
                _check_text(alias, f"aliases[{index}]", ALIAS_LIMIT_BYTES, errors)
    return errors


def canonical_lut_id(metadata: dict) -> str:
    """Return ``source:film[:print][:variant]`` for a film simulation."""
    parts = [metadata["source"]["id"], metadata["film"]["id"]]
    if "print" in metadata:
        parts.append(metadata["print"]["id"])
    if "variant" in metadata:
        parts.append(metadata["variant"])
    return ":".join(parts)


def canonical_file_name(metadata: dict) -> str:
    """Return ``film[__print][__variant].cube`` for a film simulation."""
    parts = [metadata["film"]["id"]]
    if "print" in metadata:
        parts.append(metadata["print"]["id"])
    if "variant" in metadata:
        parts.append(metadata["variant"])
    return "__".join(parts) + ".cube"


def format_metadata_line(metadata: dict) -> str:
    """Validate ``metadata`` and return its comment line without a line ending."""
    errors = validate_metadata(metadata)
    if errors:
        raise LutMetadataError("; ".join(errors))
    line = f"# {MARKER} " + json.dumps(metadata, ensure_ascii=False, separators=(",", ":"))
    if _utf8_len(line) > METADATA_LINE_LIMIT_BYTES:
        raise LutMetadataError(f"metadata line exceeds {METADATA_LINE_LIMIT_BYTES} bytes")
    return line


def _metadata_payload(line: str) -> str | None:
    """Return the JSON text of a marker line, or None for any other line."""
    text = line.strip()
    if not text.startswith("#"):
        return None
    text = text[1:].lstrip(" \t")
    if not text.startswith(MARKER):
        return None
    rest = text[len(MARKER):]
    if rest and rest[0] not in " \t":
        return None
    return rest.strip()


def _is_numeric_row(text: str) -> bool:
    return bool(text) and text[0] in _NUMERIC_START


def read_cube_header(data: bytes) -> CubeHeader:
    """Parse the header of CUBE ``data`` and stop at the first numeric row."""
    header = CubeHeader()
    offset = 3 if data.startswith(b"\xef\xbb\xbf") else 0
    found_metadata = False
    while offset < len(data):
        end = data.find(b"\n", offset)
        end = len(data) if end < 0 else end
        raw = data[offset:end]
        stripped = raw.strip()
        if _is_numeric_row(stripped.decode("ascii", errors="replace")):
            header.header_bytes = offset
            return header
        if end - (3 if data.startswith(b"\xef\xbb\xbf") else 0) > HEADER_LIMIT_BYTES:
            raise LutMetadataError(f"CUBE header exceeds {HEADER_LIMIT_BYTES} bytes")
        try:
            line = raw.decode("utf-8").rstrip("\r")
        except UnicodeDecodeError as error:
            raise LutMetadataError(f"CUBE header is not valid UTF-8: {error}") from error
        payload = _metadata_payload(line)
        if payload is not None:
            if found_metadata:
                raise LutMetadataError("CUBE header has more than one ALCEDO_LUT comment")
            if _utf8_len(line) > METADATA_LINE_LIMIT_BYTES:
                raise LutMetadataError(f"metadata line exceeds {METADATA_LINE_LIMIT_BYTES} bytes")
            try:
                metadata = json.loads(payload)
            except json.JSONDecodeError as error:
                raise LutMetadataError(f"ALCEDO_LUT JSON is not valid: {error}") from error
            errors = validate_metadata(metadata)
            if errors:
                raise LutMetadataError("; ".join(errors))
            header.metadata = metadata
            found_metadata = True
        else:
            tokens = line.strip().split()
            if len(tokens) >= 2 and tokens[0] in ("LUT_3D_SIZE", "LUT_1D_SIZE"):
                if not tokens[1].isdigit() or int(tokens[1]) < 2:
                    raise LutMetadataError(f"{tokens[0]} is not a valid size: {tokens[1]}")
                if tokens[0] == "LUT_3D_SIZE":
                    header.lut_3d_size = int(tokens[1])
                else:
                    header.lut_1d_size = int(tokens[1])
            elif tokens and tokens[0] == "TITLE":
                header.title = line.strip()[5:].strip().strip('"')
        offset = end + 1
    raise LutMetadataError("CUBE file has no numeric table")


def read_cube_header_file(path: Path) -> CubeHeader:
    """Read at most the header limit plus one line of ``path`` and parse its header."""
    with Path(path).open("rb") as source:
        data = source.read(HEADER_LIMIT_BYTES + METADATA_LINE_LIMIT_BYTES)
    return read_cube_header(data)


def insert_metadata_comment(cube: bytes, metadata: dict) -> bytes:
    """Return ``cube`` with the metadata comment as a new first line.

    Every original byte after the inserted line is kept unchanged. A file that
    already has an ALCEDO_LUT comment is rejected rather than rewritten.
    """
    existing = read_cube_header(cube)
    if existing.metadata is not None:
        raise LutMetadataError("CUBE file already has an ALCEDO_LUT comment")
    newline = b"\r\n" if b"\r\n" in cube[: existing.header_bytes or len(cube)] else b"\n"
    bom = b"\xef\xbb\xbf" if cube.startswith(b"\xef\xbb\xbf") else b""
    return bom + format_metadata_line(metadata).encode("utf-8") + newline + cube[len(bom):]
