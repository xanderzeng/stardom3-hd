#!/usr/bin/env python3
"""Change the first (root) section position in a Stardom3 GUI resource."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


SECTION_PATTERN = re.compile(rb"^\[[^\r\n]+\](?:\r?\n)", re.MULTILINE)


def replace_root_coordinate(block: bytes, key: bytes, value: int) -> bytes:
    pattern = re.compile(rb"^(?P<prefix>[ \t]*" + key + rb"[ \t]*=[ \t]*)(?P<value>-?\d+)(?P<suffix>[ \t]*(?:\r?\n|$))", re.MULTILINE)
    replacement = lambda match: match.group("prefix") + str(value).encode("ascii") + match.group("suffix")
    updated, count = pattern.subn(replacement, block, count=1)
    if count != 1:
        raise ValueError(f"root section does not contain {key.decode('ascii')} coordinate")
    return updated


def anchor_root(data: bytes, x: int, y: int) -> bytes:
    sections = list(SECTION_PATTERN.finditer(data))
    if not sections:
        raise ValueError("GUI file has no sections")
    root_start = sections[0].start()
    root_end = sections[1].start() if len(sections) > 1 else len(data)
    root = data[root_start:root_end]
    root = replace_root_coordinate(root, b"x", x)
    root = replace_root_coordinate(root, b"y", y)
    return data[:root_start] + root + data[root_end:]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--x", type=int, required=True)
    parser.add_argument("--y", type=int, required=True)
    args = parser.parse_args()

    original = args.source.read_bytes()
    anchored = anchor_root(original, args.x, args.y)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(anchored)
    print(f"{args.source} -> {args.output}: root=({args.x}, {args.y})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

