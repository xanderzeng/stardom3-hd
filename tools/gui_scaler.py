#!/usr/bin/env python3
"""Scale Stardom3 GUI resource geometry without decoding its Big5 text."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


GEOMETRY_KEYS = {
    b"x",
    b"y",
    b"width",
    b"height",
    b"fontSize",
    b"colspacing",
    b"colspace",
    b"columnSpacing",
    b"viewLeft",
    b"viewTop",
    b"viewRight",
    b"viewBottom",
    b"middleButtonSize",
    b"pageSize",
    b"sideButtonSize",
}

LINE_PATTERN = re.compile(
    rb"^(?P<prefix>[ \t]*)(?P<key>[A-Za-z][A-Za-z0-9]*)(?P<equals>[ \t]*=[ \t]*)(?P<value>-?\d+)(?P<suffix>[ \t]*(?:\r?\n|$))",
    re.MULTILINE,
)


def scale_bytes(data: bytes, factor: float) -> bytes:
    def replace(match: re.Match[bytes]) -> bytes:
        key = match.group("key")
        if key not in GEOMETRY_KEYS:
            return match.group(0)
        value = int(match.group("value"))
        if value == 0:
            scaled = 0
        else:
            scaled = round(value * factor)
            if value > 0:
                scaled = max(1, scaled)
        return b"".join(
            (
                match.group("prefix"),
                key,
                match.group("equals"),
                str(scaled).encode("ascii"),
                match.group("suffix"),
            )
        )

    return LINE_PATTERN.sub(replace, data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()

    if args.height < 600:
        parser.error("height must be at least 600")
    factor = args.height / 600.0

    for relative_name in args.files:
        relative = Path(relative_name)
        source = (args.source / relative).resolve()
        output = (args.output / relative).resolve()
        if not source.is_file():
            raise FileNotFoundError(source)
        output.parent.mkdir(parents=True, exist_ok=True)
        original = source.read_bytes()
        scaled = scale_bytes(original, factor)
        output.write_bytes(scaled)
        print(f"{relative}: {len(original)} bytes -> {len(scaled)} bytes, scale={factor:.3f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

