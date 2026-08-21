#!/usr/bin/env python3
"""Insert a transparent proxy root and offset the original Stardom3 GUI root."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


SECTION_PATTERN = re.compile(rb"^\[(?P<name>[^\r\n]+)\](?:\r?\n)", re.MULTILINE)


def replace_value(block: bytes, key: bytes, value: bytes) -> bytes:
    pattern = re.compile(
        rb"^(?P<prefix>[ \t]*" + re.escape(key) + rb"[ \t]*=[ \t]*)(?P<value>[^\r\n]*)(?P<suffix>\r?\n|$)",
        re.MULTILINE,
    )
    updated, count = pattern.subn(
        lambda match: match.group("prefix") + value + match.group("suffix"), block, count=1
    )
    if count != 1:
        raise ValueError(f"root section does not contain {key.decode('ascii')}")
    return updated


def optional_replace(block: bytes, key: bytes, value: bytes) -> bytes:
    try:
        return replace_value(block, key, value)
    except ValueError:
        return block


def wrap_root(data: bytes, x: int, y: int) -> bytes:
    sections = list(SECTION_PATTERN.finditer(data))
    if not sections:
        raise ValueError("GUI file has no sections")
    root_start = sections[0].start()
    root_end = sections[1].start() if len(sections) > 1 else len(data)
    root = data[root_start:root_end]
    root_name = sections[0].group("name")
    newline = b"\r\n" if b"\r\n" in root else b"\n"

    wrapper = re.sub(
        rb"^\[" + re.escape(root_name) + rb"\]",
        b"[WidescreenRoot]",
        root,
        count=1,
    )
    wrapper = replace_value(wrapper, b"x", b"0")
    wrapper = replace_value(wrapper, b"y", b"0")
    wrapper = optional_replace(wrapper, b"borderType", b"0")
    wrapper = optional_replace(wrapper, b"borderImageNormal", b"0")
    wrapper = optional_replace(wrapper, b"borderImageMove", b"0")
    wrapper = optional_replace(wrapper, b"borderImagePress", b"0")
    wrapper = optional_replace(wrapper, b"borderImageNull", b"0")
    wrapper = optional_replace(wrapper, b"rectCheckOnCursor", b"false")
    wrapper = optional_replace(wrapper, b"limitMove", b"false")

    child = replace_value(root, b"parent", b"WidescreenRoot")
    child = replace_value(child, b"x", str(x).encode("ascii"))
    child = replace_value(child, b"y", str(y).encode("ascii"))
    return data[:root_start] + wrapper.rstrip(b"\r\n") + newline * 2 + child + data[root_end:]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--x", type=int, required=True)
    parser.add_argument("--y", type=int, required=True)
    args = parser.parse_args()

    result = wrap_root(args.source.read_bytes(), args.x, args.y)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    print(f"{args.source} -> {args.output}: proxy child offset=({args.x}, {args.y})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
