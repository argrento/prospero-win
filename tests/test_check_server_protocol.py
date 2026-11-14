#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/check_server_protocol.py accepts a patched protocol that only adds to stock Wine's, and refuses one that
renumbers a request (as the first version of patch 1021 did by adding release_fd_loan after get_handle_fd, or a
hand-edited enum line), moves, drops or narrows a field, shrinks a request, or changes a shared type, even when its
size stays the same."""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_server_protocol  # noqa: E402

STOCK_REQUESTS = ["new_process", "get_handle_fd", "get_directory_cache_entry", "get_kernel_object_ptr"]
STOCK_LAYOUT = """C_ASSERT( sizeof(struct rectangle) == 16 );
C_ASSERT( sizeof(union apc_call) == 64 );
C_ASSERT( sizeof(unsigned int) == 4 );
C_ASSERT( offsetof(struct get_handle_fd_request, handle) == 12 );
C_ASSERT( sizeof(struct get_handle_fd_request) == 16 );
C_ASSERT( offsetof(struct get_handle_fd_reply, type) == 8 );
C_ASSERT( offsetof(struct get_handle_fd_reply, options) == 20 );
C_ASSERT( offsetof(struct get_handle_fd_reply, __pad_24) == 24 );
C_ASSERT( sizeof(struct get_handle_fd_reply) == 24 );
"""


STOCK_DEFINITIONS = """struct rectangle
{
    int  left;
    int  top;
    int  right;
    int  bottom;
};

struct get_handle_fd_request
{
    struct request_header __header;
    obj_handle_t handle;
};
struct get_handle_fd_reply
{
    struct reply_header __header;
    int          type;
    unsigned int options;
    char __pad_20[4];
};
typedef union
{
    int type;
    struct
    {
        int  type;
        obj_handle_t handle;
    } wait;
} select_op_t;
"""


def tree(root: Path, name: str, requests: list[str], layout: str, definitions: str = STOCK_DEFINITIONS,
         enum: str | None = None) -> Path:
    path = root / name
    (path / "include/wine").mkdir(parents=True)
    (path / "server").mkdir()
    enum = enum if enum is not None else "".join(f"    REQ_{request},\n" for request in requests)
    (path / "include/wine/server_protocol.h").write_text(
        f"{definitions}\nenum request\n{{\n{enum}    REQ_NB_REQUESTS\n}};\n")
    (path / "server/request_handlers.h").write_text(layout)
    return path


def check(root: Path, name: str, requests: list[str], layout: str, **header) -> list[str]:
    stock = root / "stock"
    if not stock.exists():
        tree(root, "stock", STOCK_REQUESTS, STOCK_LAYOUT)
    try:
        return check_server_protocol.problems(stock, tree(root, name, requests, layout, **header))
    except ValueError as error:
        return [str(error)]


def main() -> int:
    failures = []

    def expect(label: str, found: list[str], needle: str | None) -> None:
        if needle is None and found:
            failures.append(f"{label}: refused: {found}")
        elif needle is not None and not any(needle in problem for problem in found):
            failures.append(f"{label}: expected a problem naming {needle!r}, got {found}")

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        appended = STOCK_LAYOUT.replace(
            "C_ASSERT( offsetof(struct get_handle_fd_reply, __pad_24) == 24 );\n"
            "C_ASSERT( sizeof(struct get_handle_fd_reply) == 24 );\n",
            "C_ASSERT( offsetof(struct get_handle_fd_reply, loan) == 24 );\n"
            "C_ASSERT( sizeof(struct get_handle_fd_reply) == 32 );\n")
        expect("same", check(root, "same", STOCK_REQUESTS, STOCK_LAYOUT), None)
        expect("request and field added at the ends",
               check(root, "added", STOCK_REQUESTS + ["release_fd_loan"], appended), None)
        expect("request added in the middle (patch 1021's first version)",
               check(root, "middle", STOCK_REQUESTS[:2] + ["release_fd_loan"] + STOCK_REQUESTS[2:], appended),
               "request 2 is release_fd_loan")
        expect("request removed", check(root, "removed", STOCK_REQUESTS[:-1], STOCK_LAYOUT), "was removed")
        expect("field moved", check(root, "moved", STOCK_REQUESTS, STOCK_LAYOUT.replace(
            "get_handle_fd_reply, options) == 20", "get_handle_fd_reply, options) == 24")),
            "options moved from offset 20 to 24")
        expect("field dropped", check(root, "dropped", STOCK_REQUESTS, STOCK_LAYOUT.replace(
            "C_ASSERT( offsetof(struct get_handle_fd_reply, type) == 8 );\n", "")), "type is gone")
        expect("request shrank", check(root, "shrank", STOCK_REQUESTS, STOCK_LAYOUT.replace(
            "sizeof(struct get_handle_fd_request) == 16", "sizeof(struct get_handle_fd_request) == 8")),
            "shrank from 16 to 8")
        expect("shared type grew", check(root, "grew", STOCK_REQUESTS, STOCK_LAYOUT.replace(
            "sizeof(union apc_call) == 64", "sizeof(union apc_call) == 72")), "union apc_call is 72 bytes")
        expect("no assertions", check(root, "empty", STOCK_REQUESTS, ""), "is gone")
        expect("member added at the end of a reply", check(root, "member", STOCK_REQUESTS, appended,
               definitions=STOCK_DEFINITIONS.replace("    char __pad_20[4];\n", "    unsigned int loan;\n")), None)
        expect("field narrowed, offsets and sizes the same", check(root, "narrowed", STOCK_REQUESTS, STOCK_LAYOUT,
               definitions=STOCK_DEFINITIONS.replace("    obj_handle_t handle;\n};",
                                                     "    short handle;\n    char __pad_14[2];\n};")),
               "stock member 'obj_handle_t handle;' is now 'short handle;'")
        expect("shared struct reordered, size the same", check(root, "reordered", STOCK_REQUESTS, STOCK_LAYOUT,
               definitions=STOCK_DEFINITIONS.replace("    int  left;\n    int  top;", "    int  top;\n    int  left;")),
               "rectangle is defined differently")
        expect("union variant changed", check(root, "variant", STOCK_REQUESTS, STOCK_LAYOUT,
               definitions=STOCK_DEFINITIONS.replace("        obj_handle_t handle;\n    } wait;",
                                                     "        unsigned int handle;\n    } wait;")),
               "select_op_t is defined differently")
        expect("struct removed", check(root, "removed-struct", STOCK_REQUESTS, STOCK_LAYOUT,
               definitions=STOCK_DEFINITIONS.split("struct get_handle_fd_request")[0]),
               "get_handle_fd_request is no longer defined")
        expect("hand-edited enum line", check(root, "tab", STOCK_REQUESTS, STOCK_LAYOUT,
               enum="    REQ_new_process,\n\tREQ_inserted ,\n    REQ_get_handle_fd,\n"
                    "    REQ_get_directory_cache_entry,\n    REQ_get_kernel_object_ptr,\n"),
               "request 1 is inserted")
        expect("enum line that isn't a request", check(root, "junk", STOCK_REQUESTS, STOCK_LAYOUT,
               enum="    REQ_new_process,\n    REQ_get_handle_fd, REQ_inserted,\n"),
               "a line that isn't a request")
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        return 1
    print("check server protocol: 16 cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
