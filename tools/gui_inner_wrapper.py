#!/usr/bin/env python3
"""Keep Stardom3's required GUI root and offset its visual/input content as a child."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


SECTION_PATTERN = re.compile(rb"^\[(?P<name>[^\r\n]+)\](?:\r?\n)", re.MULTILINE)


def replace_value(block: bytes, key: bytes, value: bytes, required: bool = True) -> bytes:
    pattern = re.compile(
        rb"^(?P<prefix>[ \t]*" + re.escape(key) + rb"[ \t]*=[ \t]*)(?P<value>[^\r\n]*)(?P<suffix>\r?\n|$)",
        re.MULTILINE,
    )
    updated, count = pattern.subn(
        lambda match: match.group("prefix") + value + match.group("suffix"), block, count=1
    )
    if required and count != 1:
        raise ValueError(f"section does not contain {key.decode('ascii')}")
    return updated


def inner_wrap(data: bytes, x: int, y: int) -> bytes:
    sections = list(SECTION_PATTERN.finditer(data))
    if not sections:
        raise ValueError("GUI file has no sections")
    root_start = sections[0].start()
    root_end = sections[1].start() if len(sections) > 1 else len(data)
    root = data[root_start:root_end]
    root_name = sections[0].group("name")
    newline = b"\r\n" if b"\r\n" in root else b"\n"

    container = root
    container = replace_value(container, b"color", b"00ffffff", required=False)
    container = replace_value(container, b"borderType", b"0", required=False)
    container = replace_value(container, b"borderColor", b"0", required=False)
    for key in (b"borderImageNormal", b"borderImageMove", b"borderImagePress", b"borderImageNull"):
        container = replace_value(container, key, b"0", required=False)
    container = replace_value(container, b"rectCheckOnCursor", b"false", required=False)
    container = replace_value(container, b"limitMove", b"false", required=False)

    content = re.sub(
        rb"^\[" + re.escape(root_name) + rb"\]",
        b"[WidescreenContent]",
        root,
        count=1,
    )
    content = replace_value(content, b"parent", root_name)
    content = replace_value(content, b"x", str(x).encode("ascii"))
    content = replace_value(content, b"y", str(y).encode("ascii"))

    remainder = data[root_end:]
    parent_pattern = re.compile(
        rb"^(?P<prefix>[ \t]*parent[ \t]*=[ \t]*)" + re.escape(root_name) + rb"(?P<suffix>[ \t]*(?:\r?\n|$))",
        re.MULTILINE,
    )
    remainder = parent_pattern.sub(
        lambda match: match.group("prefix") + b"WidescreenContent" + match.group("suffix"),
        remainder,
    )
    return (
        data[:root_start]
        + container.rstrip(b"\r\n")
        + newline * 2
        + content.rstrip(b"\r\n")
        + newline * 2
        + remainder.lstrip(b"\r\n")
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--x", type=int, required=True)
    parser.add_argument("--y", type=int, required=True)
    args = parser.parse_args()

    result = inner_wrap(args.source.read_bytes(), args.x, args.y)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    print(f"{args.source} -> {args.output}: inner content offset=({args.x}, {args.y})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
