#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Writes the generated parts of the alcedo-cli command reference.

The command schemas (`session.describe`) and the editor field catalog (`editor.catalog`) are
committed as JSON in skills/alcedo/references/reference/ of the alcedo skill repository
(https://github.com/zidage/alcedo-skills). This repository does not pin a commit of it: pass the
path of a local checkout with --skills-dir. AutomationReferenceDocsTest, registered when CMake has
ALCEDO_SKILLS_DIR, checks that these files are the same as the schemas of the current build.
This script writes, from those files:

- the block between `<!-- BEGIN GENERATED: <method> -->` and `<!-- END GENERATED: <method> -->`
  in the command pages (description, parameters, result);
- the block between `<!-- BEGIN GENERATED: editor-fields -->` and
  `<!-- END GENERATED: editor-fields -->` in editor_fields.md.

Usage (every form needs --skills-dir <alcedo-skills checkout>):
  generate_cli_reference.py            Rewrite the generated blocks.
  generate_cli_reference.py --check    Fail when a block differs, a command has no block, or a
                                       block names an unknown command.
  generate_cli_reference.py --schema <commands.json> --catalog <editor_catalog.json>
                                       Copy new schema files into the reference first (the
                                       failure message of AutomationReferenceDocsTest names them).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# The command reference inside an alcedo-skills checkout.
REFERENCE_SUBDIR = Path("skills") / "alcedo" / "references" / "reference"
FIELDS_BLOCK = "editor-fields"

BLOCK_PATTERN = re.compile(
    r"(<!-- BEGIN GENERATED: (?P<name>[A-Za-z0-9._-]+) -->\n)(?P<body>.*?)"
    r"(<!-- END GENERATED: (?P=name) -->)",
    re.DOTALL,
)

# Parameters that every command accepts; cli.md describes them once.
COMMON_PARAMETERS = {"timeout_ms"}


def cell(text: str) -> str:
    """Makes text safe for one Markdown table cell."""
    return text.replace("|", "\\|").replace("\n", " ").strip()


def number(value) -> str:
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def type_text(schema: dict) -> str:
    if "enum" in schema:
        return " | ".join(f"`{json.dumps(value)}`" if not isinstance(value, str)
                          else f"`{value}`" for value in schema["enum"])
    kind = schema.get("type")
    if isinstance(kind, list):
        return " or ".join(kind)
    if kind == "array":
        items = schema.get("items", {})
        return f"array of {type_text(items)}" if items else "array"
    return kind or "any"


def limits_text(schema: dict) -> str:
    parts = []
    if "minimum" in schema and "maximum" in schema:
        parts.append(f"{number(schema['minimum'])} to {number(schema['maximum'])}")
    elif "minimum" in schema:
        parts.append(f"at least {number(schema['minimum'])}")
    elif "maximum" in schema:
        parts.append(f"at most {number(schema['maximum'])}")
    if "minItems" in schema and "maxItems" in schema:
        parts.append(f"{schema['minItems']} to {schema['maxItems']} items")
    elif "minItems" in schema:
        parts.append(f"at least {schema['minItems']} item(s)")
    elif "maxItems" in schema:
        parts.append(f"at most {schema['maxItems']} items")
    items = schema.get("items", {})
    if "minimum" in items:
        parts.append(f"each item at least {number(items['minimum'])}")
    return "; ".join(parts)


def flatten(properties: dict, required: list, prefix: str = ""):
    """Yields (name, schema, required) for each property and the properties of its objects."""
    for name in sorted(properties):
        schema = properties[name]
        if not prefix and name in COMMON_PARAMETERS:
            continue
        full_name = f"{prefix}{name}"
        yield full_name, schema, name in required
        if schema.get("type") == "object" and "properties" in schema:
            yield from flatten(schema["properties"], schema.get("required", []), full_name + ".")
        items = schema.get("items", {})
        if schema.get("type") == "array" and items.get("type") == "object" and \
                "properties" in items:
            yield from flatten(items["properties"], items.get("required", []), full_name + "[].")


def notes_text(schema: dict) -> str:
    parts = [part.rstrip(".") for part in (limits_text(schema), schema.get("description", ""))
             if part]
    return ". ".join(parts) + ("." if parts else "")


def parameters_table(schema: dict) -> list[str]:
    rows = list(flatten(schema.get("properties", {}), schema.get("required", [])))
    if not rows:
        return ["No parameters other than `timeout_ms`."]
    lines = ["| Name | Type | Required | Default | Notes |", "| --- | --- | --- | --- | --- |"]
    for name, property_schema, required in rows:
        default = property_schema.get("default")
        notes = notes_text(property_schema)
        lines.append(
            f"| `{name}` | {cell(type_text(property_schema))} | {'yes' if required else 'no'} | "
            f"{'' if default is None else '`' + json.dumps(default) + '`'} | {cell(notes)} |")
    return lines


def result_table(schema: dict) -> list[str]:
    rows = list(flatten(schema.get("properties", {}), schema.get("required", [])))
    if not rows:
        return ["The schema lists no fields; see the description and the example."]
    lines = ["| Field | Type | Always | Notes |", "| --- | --- | --- | --- |"]
    for name, property_schema, required in rows:
        notes = notes_text(property_schema)
        lines.append(f"| `{name}` | {cell(type_text(property_schema))} | "
                     f"{'yes' if required else 'no'} | {cell(notes)} |")
    return lines


def command_block(command: dict) -> str:
    state = ("yes. In a GUI session it needs agent control (`control.acquire`)."
             if command["changes_state"] else "no.")
    lines = [
        f"Changes state: {state}",
        "",
        command["description"],
        "",
        "**Parameters**",
        "",
        *parameters_table(command["params_schema"]),
        "",
        "**Result**",
        "",
        *result_table(command["result_schema"]),
        "",
    ]
    return "\n".join(lines)


def range_text(entry: dict) -> str:
    if "ui_min" in entry and "ui_max" in entry:
        return f"{number(entry['ui_min'])} to {number(entry['ui_max'])}"
    return ""


def fields_block(catalog: dict) -> str:
    fields = catalog["fields"]
    lines = ["### Scalar fields", "",
             "| Field | Aliases | Panel | Range | Step | Decimals | Reset value |",
             "| --- | --- | --- | --- | --- | --- | --- |"]
    for entry in fields:
        if entry["kind"] != "scalar":
            continue
        aliases = ", ".join(f"`{alias}`" for alias in entry.get("aliases", []))
        lines.append(f"| `{entry['field']}` | {aliases} | {entry['panel']} | {range_text(entry)} | "
                     f"{number(entry.get('ui_step', ''))} | {entry.get('ui_decimals', '')} | "
                     f"{number(entry['ui_default'])} |")
    lines.append("")
    lines += ["### Object fields", "",
              "A write can give some of the properties; the others keep their values.", ""]
    for entry in fields:
        if entry["kind"] != "object":
            continue
        lines += [f"#### `{entry['field']}` ({entry['panel']} panel)", "",
                  "| Property | Type | Range or values | Step | Decimals |",
                  "| --- | --- | --- | --- | --- |"]
        for prop in entry.get("properties", []):
            if prop["type"] == "option":
                values = ", ".join(f"`{option['value']}` ({option['label']})"
                                   for option in prop.get("options", []))
            elif prop["type"] == "number_list":
                values = f"{prop.get('ui_count', '')} numbers, {range_text(prop)}"
            else:
                values = range_text(prop)
            if "ui_slider" in prop:
                slider = prop["ui_slider"]
                values += (f"; slider scale `{slider['scale']}`, position "
                           f"{slider['position_min']} to {slider['position_max']}, "
                           f"{number(slider['pivot_kelvin'])} K at {slider['pivot_position']}")
            lines.append(f"| `{prop['name']}` | {prop['type']} | {cell(values)} | "
                         f"{number(prop.get('ui_step', ''))} | {prop.get('ui_decimals', '')} |")
        lines += ["", f"Reset value: `{json.dumps(entry['ui_default'], sort_keys=True)}`", ""]
        if "encoding_eotf_by_space" in entry:
            lines += ["Valid `encoding_eotf` values of each `encoding_space`:", ""]
            for space, eotfs in sorted(entry["encoding_eotf_by_space"].items()):
                lines.append(f"- `{space}`: " + ", ".join(f"`{eotf}`" for eotf in eotfs))
            lines.append("")
    lines += ["### Model fields", "", "A write gives the complete value.", ""]
    for entry in fields:
        if entry["kind"] != "model":
            continue
        lines += [f"#### `{entry['field']}` ({entry['panel']} panel)", "",
                  entry.get("model_shape", ""), "",
                  f"Reset value: `{json.dumps(entry['ui_default'], sort_keys=True)}`", ""]
    return "\n".join(lines)


def expected_blocks(commands: list, catalog: dict) -> dict[str, str]:
    blocks = {command["method"]: command_block(command) for command in commands}
    blocks[FIELDS_BLOCK] = fields_block(catalog)
    return blocks


def copy_json(source: Path, target: Path) -> None:
    """Writes the JSON of @p source to @p target with two-space indents and LF line ends, the form
    of the committed files. The build writes four-space indents."""
    data = json.loads(source.read_text(encoding="utf-8"))
    target.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                      newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--skills-dir", type=Path, required=True,
                        help="A local checkout of https://github.com/zidage/alcedo-skills.")
    parser.add_argument("--check", action="store_true", help="Check only; change no file.")
    parser.add_argument("--schema", type=Path, help="A new commands.json to copy first.")
    parser.add_argument("--catalog", type=Path, help="A new editor_catalog.json to copy first.")
    arguments = parser.parse_args()

    reference_dir = arguments.skills_dir / REFERENCE_SUBDIR
    if not reference_dir.is_dir():
        print(f"{reference_dir} does not exist; --skills-dir must name an alcedo-skills checkout",
              file=sys.stderr)
        return 1
    commands_json = reference_dir / "commands.json"
    catalog_json = reference_dir / "editor_catalog.json"

    if arguments.schema:
        copy_json(arguments.schema, commands_json)
    if arguments.catalog:
        copy_json(arguments.catalog, catalog_json)

    commands = json.loads(commands_json.read_text(encoding="utf-8"))
    catalog = json.loads(catalog_json.read_text(encoding="utf-8"))
    expected = expected_blocks(commands, catalog)

    problems = []
    seen: dict[str, Path] = {}
    for page in sorted(reference_dir.glob("*.md")):
        text = page.read_text(encoding="utf-8")

        def replace(match: re.Match) -> str:
            name = match.group("name")
            if name in seen:
                problems.append(f"{page.name}: '{name}' has a second block (first in "
                                f"{seen[name].name})")
                return match.group(0)
            seen[name] = page
            if name not in expected:
                problems.append(f"{page.name}: '{name}' is not a command of commands.json")
                return match.group(0)
            if match.group("body") != expected[name]:
                problems.append(f"{page.name}: the block of '{name}' is not up to date")
            return match.group(1) + expected[name] + match.group(4)

        new_text = BLOCK_PATTERN.sub(replace, text)
        if not arguments.check and new_text != text:
            page.write_text(new_text, encoding="utf-8", newline="\n")

    for name in expected:
        if name not in seen:
            problems.append(f"'{name}' has no generated block in the reference pages")

    if arguments.check:
        for problem in problems:
            print(problem, file=sys.stderr)
        if problems:
            print("Run scripts/automation/generate_cli_reference.py --skills-dir <checkout> "
                  "and add the missing sections.", file=sys.stderr)
            return 1
        return 0

    missing = [problem for problem in problems if "not up to date" not in problem]
    for problem in missing:
        print(problem, file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
