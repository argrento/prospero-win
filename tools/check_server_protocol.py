#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that a patched Wine tree keeps stock Wine's server protocol, only adding to it.

A Wine module sends each server request by number, its place in enum request, and lays the request and its reply
out as the generated structures say. A module the patches don't rebuild keeps stock Wine's numbers and layouts, so
the patched protocol may only add: new requests after every stock one, and new fields after a stock structure's own.

Read from the generated headers of both trees:
- include/wine/server_protocol.h: stock's enum request must be the start of the patched one (every line of the enum
  must be a request, so a hand-edited entry can't hide); a stock request or reply keeps its members, in order, as
  declared, and may only get new ones at its end; every other stock struct or union (struct rectangle,
  union apc_call, ...) keeps its definition as it is;
- server/request_handlers.h: every C_ASSERT of a stock field offset must hold unchanged, a stock request or reply
  may only grow, and every other stock type (struct rectangle, union apc_call, ...) must keep its size.

Usage: tools/check_server_protocol.py STOCK_TREE PATCHED_TREE
Exits 1 and names each difference when the patched protocol breaks stock modules."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ENUM_LINE = re.compile(r"^\s*REQ_(\w+)\s*,\s*$")
BLOCK = re.compile(r"^(?:typedef )?(?:struct|union)(?: (\w+))?$")
OFFSET = re.compile(r"^C_ASSERT\( offsetof\(struct (\w+), (\w+)\) == (\d+) \);$", re.M)
SIZE = re.compile(r"^C_ASSERT\( sizeof\(([\w ]+?)\) == (\d+) \);$", re.M)
MESSAGE = re.compile(r"^struct \w+_(request|reply)$")


def requests(header: str) -> list[str]:
    start = header.index("enum request\n{") + len("enum request\n{")
    names = []
    for line in header[start:header.index("REQ_NB_REQUESTS", start)].splitlines():
        if not line.strip():
            continue
        match = ENUM_LINE.match(line)
        if not match:
            raise ValueError(f"enum request has a line that isn't a request: {line!r}")
        names.append(match.group(1))
    return names


def definitions(header: str) -> dict[str, list[str]]:
    """Each struct or union the header defines, by name, as its member lines with spaces collapsed."""
    lines = header.splitlines()
    found: dict[str, list[str]] = {}
    index = 0
    while index < len(lines) - 1:
        match = BLOCK.match(lines[index])
        if not match or lines[index + 1] != "{":
            index += 1
            continue
        depth, body, end = 0, [], index + 1
        for end in range(index + 1, len(lines)):
            line = lines[end]
            depth += line.count("{") - line.count("}")
            if end > index + 1 and depth > 0:
                body.append(" ".join(line.split()))
            if depth == 0:
                break
        name = match.group(1) or re.match(r"^}\s*(\w+)\s*;", lines[end].strip()).group(1)
        found[name] = body
        index = end + 1
    return found


def layouts(handlers: str) -> tuple[dict[tuple[str, str], int], dict[str, int], dict[str, int]]:
    """Field offsets in each request and reply, their sizes, and the sizes of every other type."""
    offsets = {(struct, field): int(value) for struct, field, value in OFFSET.findall(handlers)
               if not field.startswith("__pad")}
    messages: dict[str, int] = {}
    types: dict[str, int] = {}
    for name, value in SIZE.findall(handlers):
        (messages if MESSAGE.match(name) else types)[name] = int(value)
    return offsets, messages, types


def problems(stock_tree: Path, patched_tree: Path) -> list[str]:
    found = []
    header = "include/wine/server_protocol.h"
    stock_requests = requests((stock_tree / header).read_text())
    patched_requests = requests((patched_tree / header).read_text())
    if patched_requests[:len(stock_requests)] != stock_requests:
        for number, (old, new) in enumerate(zip(stock_requests, patched_requests)):
            if old != new:
                found.append(f"request {number} is {new}, stock Wine's is {old}: add new requests at the end "
                             "of server/protocol.def")
                break
        else:
            found.append(f"the patched protocol has {len(patched_requests)} requests, stock Wine's "
                         f"{len(stock_requests)}: a stock request was removed")
    stock_definitions = definitions((stock_tree / header).read_text())
    patched_definitions = definitions((patched_tree / header).read_text())
    for name, members in sorted(stock_definitions.items()):
        new = patched_definitions.get(name)
        if new is None:
            found.append(f"{name} is no longer defined")
        elif name.endswith(("_request", "_reply")):
            old = [member for member in members if "__pad" not in member]
            kept = [member for member in new if "__pad" not in member][:len(old)]
            if kept != old:
                changed = next((a, b) for a, b in zip(old + [""], kept + [""]) if a != b)
                found.append(f"{name}: stock member {changed[0]!r} is now {changed[1]!r}: add new members at "
                             "the end")
        elif new != members:
            found.append(f"{name} is defined differently from stock Wine's")
    handlers = "server/request_handlers.h"
    stock_offsets, stock_structs, stock_types = layouts((stock_tree / handlers).read_text())
    offsets, structs, types = layouts((patched_tree / handlers).read_text())
    if not stock_offsets or not stock_structs:
        found.append(f"no layout assertions in stock Wine's {handlers}")
    for (struct, field), value in sorted(stock_offsets.items()):
        if (struct, field) not in offsets:
            found.append(f"{struct}.{field} is gone (stock offset {value})")
        elif offsets[struct, field] != value:
            found.append(f"{struct}.{field} moved from offset {value} to {offsets[struct, field]}")
    for struct, size in sorted(stock_structs.items()):
        if struct not in structs:
            found.append(f"{struct} is gone")
        elif structs[struct] < size:
            found.append(f"{struct} shrank from {size} to {structs[struct]} bytes")
    for name, size in sorted(stock_types.items()):
        if types.get(name) != size:
            found.append(f"{name} is {types.get(name)} bytes, stock Wine's is {size}")
    return found


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.split("\n\n")[-1], file=sys.stderr)
        return 2
    try:
        found = problems(Path(argv[0]), Path(argv[1]))
    except ValueError as error:
        found = [str(error)]
    for problem in found:
        print(f"server protocol: {problem}", file=sys.stderr)
    if found:
        print("server protocol: modules built from stock Wine would send the wrong requests", file=sys.stderr)
        return 1
    print("server protocol: stock Wine's requests and layouts are unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
