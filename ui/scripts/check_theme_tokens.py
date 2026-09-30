#!/usr/bin/env python3
"""Check that the Qt palette adapter matches DESIGN.md's normative colors."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
DESIGN = ROOT / "DESIGN.md"
TOKENS = ROOT / "ui" / "src" / "theme_tokens.hpp"


def yaml_scalar_section(text: str, section_name: str) -> dict[str, str]:
    tokens: dict[str, str] = {}
    in_section = False
    for line in text.splitlines():
        if line == f"{section_name}:":
            in_section = True
            continue
        if in_section and line and not line.startswith(" "):
            break
        if in_section:
            match = re.fullmatch(r'  ([A-Za-z][A-Za-z0-9-]*): "([^"]+)"', line)
            if match:
                tokens[match.group(1).replace("-", "_").lower()] = match.group(2)
    return tokens


def cpp_colors(text: str) -> dict[str, str]:
    return {
        name: value.upper()
        for name, value in re.findall(
            r'inline constexpr char ([a-z_]+)\[\] = "(#[0-9A-Fa-f]{6})";', text
        )
    }


def cpp_geometry(text: str) -> dict[str, str]:
    return {
        name: value
        for name, value in re.findall(r"inline constexpr int ([a-z_]+) = (\d+);", text)
    }


def main() -> int:
    try:
        design = DESIGN.read_text(encoding="utf-8")
        adapter_text = TOKENS.read_text(encoding="utf-8")
        source = yaml_scalar_section(design, "colors")
        geometry = yaml_scalar_section(design, "spacing")
        for key, value in yaml_scalar_section(design, "rounded").items():
            geometry[f"radius_{key}" if key != "default" else "radius_default"] = value
        adapter = cpp_colors(adapter_text)
        adapter_geometry = cpp_geometry(adapter_text)
    except OSError as error:
        print(f"theme token check could not read an input: {error}", file=sys.stderr)
        return 2

    if source != adapter:
        missing = sorted(source.keys() - adapter.keys())
        extra = sorted(adapter.keys() - source.keys())
        changed = sorted(key for key in source.keys() & adapter.keys() if source[key] != adapter[key])
        for key in missing:
            print(f"Qt theme is missing DESIGN.md color '{key}'", file=sys.stderr)
        for key in extra:
            print(f"Qt theme has an undocumented color '{key}'", file=sys.stderr)
        for key in changed:
            print(f"Qt theme color '{key}' differs: {adapter[key]} != {source[key]}", file=sys.stderr)
        return 1

    design_geometry: dict[str, str] = {}
    key_map = {
        "panel": "panel_inset",
        "cluster": "cluster_gap",
        "control": "control_height",
        "dense_control": "dense_control_height",
    }
    for key, value in geometry.items():
        name = key_map.get(key, key)
        match = re.fullmatch(r"(\d+)px", value)
        if match:
            design_geometry[name] = match.group(1)

    if design_geometry != adapter_geometry:
        missing = sorted(design_geometry.keys() - adapter_geometry.keys())
        extra = sorted(adapter_geometry.keys() - design_geometry.keys())
        changed = sorted(
            key for key in design_geometry.keys() & adapter_geometry.keys()
            if design_geometry[key] != adapter_geometry[key]
        )
        for key in missing:
            print(f"Qt theme is missing DESIGN.md dimension '{key}'", file=sys.stderr)
        for key in extra:
            print(f"Qt theme has an undocumented dimension '{key}'", file=sys.stderr)
        for key in changed:
            print(f"Qt dimension '{key}' differs: {adapter_geometry[key]} != {design_geometry[key]}", file=sys.stderr)
        return 1

    print(f"Qt theme matches {len(source)} DESIGN.md colors and {len(design_geometry)} dimensions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
