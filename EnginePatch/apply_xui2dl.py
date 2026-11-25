#!/usr/bin/env python3
"""Apply, check or remove the XUI2DL engine patch (docs/design/M11-xui2dl.md section 3).

    python apply_xui2dl.py <engine root>            copy the XUI2DL files and insert the hooks
    python apply_xui2dl.py <engine root> --check    report the state, write nothing
    python apply_xui2dl.py <engine root> --remove   remove the hooks (the copied files stay)

Every hook is wrapped in "// XUI2DL begin" / "// XUI2DL end" ("#" in CMake). A hook that is already there is
left alone. When an anchor is missing or ambiguous nothing is written at all. Standard library only.

Exit codes: 0 = done / fully applied (--check), 1 = --check found something missing or different, 2 = error.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# (source relative to EnginePatch/, destination relative to the engine root)
FILES = [
    ("Code/CryEngine/RenderDll/XUI2DL/XUI2DLApi.h", "Code/CryEngine/RenderDll/XUI2DL/XUI2DLApi.h"),
    ("Code/CryEngine/RenderDll/XUI2DL/XUI2DL.h", "Code/CryEngine/RenderDll/XUI2DL/XUI2DL.h"),
    ("Code/CryEngine/RenderDll/XUI2DL/XUI2DL.cpp", "Code/CryEngine/RenderDll/XUI2DL/XUI2DL.cpp"),
    ("Code/CryEngine/RenderDll/XUI2DL/NOTICE.md", "Code/CryEngine/RenderDll/XUI2DL/NOTICE.md"),
    ("Engine/Shaders/HWScripts/CryFX/XUI2DL.cfx", "Engine/Shaders/HWScripts/CryFX/XUI2DL.cfx"),
]

RD = "Code/CryEngine/RenderDll/XRenderD3D9/"


class AnchorError(Exception):
    pass


def find_one(lines, pattern, start=0, end=None, what=""):
    """Index of the only line in [start, end) matching `pattern`; AnchorError when none or several."""
    rx = re.compile(pattern)
    end = len(lines) if end is None else end
    hits = [i for i in range(start, end) if rx.search(lines[i])]
    if len(hits) != 1:
        raise AnchorError("%s: %d line(s) match %r (exactly one expected)" % (what, len(hits), pattern))
    return hits[0]


def indent_of(line):
    return line[: len(line) - len(line.lstrip(" \t"))]


# Each hook: (name, comment prefix, body lines, locate(lines) -> insertion index before which the block goes).

def loc_driver_include(lines):
    return find_one(lines, r'^\s*#include\s+"D3DPostProcess\.h"', what="DriverD3D.cpp include anchor") + 1


def loc_driver_endframe(lines):
    # With XUI2D: before the XUI2D::EndFrame( line; without: before the RenderAux() call. Both inside EndFrame().
    start = find_one(lines, r'^void\s+CD3D9Renderer::EndFrame\s*\(\s*\)', what="CD3D9Renderer::EndFrame")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].rstrip("\r\n") == "}"), None)
    if end is None:
        raise AnchorError("CD3D9Renderer::EndFrame: no closing brace")
    rx = re.compile(r"^\s*XUI2D::EndFrame\(")
    if any(rx.search(lines[i]) for i in range(start, end)):
        return find_one(lines, r"^\s*XUI2D::EndFrame\(", start, end, what="EndFrame XUI2D::EndFrame( anchor")
    return find_one(lines, r"^\s*RenderAux\(\);", start, end, what="EndFrame RenderAux() anchor")


def loc_thread_include(lines):
    return find_one(lines, r"^\s*#include\s+<CrySystem/VR/IHMDManager\.h>", what="D3DRenderThread.cpp include anchor") + 1


def loc_thread_create(lines):
    start = find_one(lines, r"^void\s+CD3D9Renderer::RT_CreateRenderResources\s*\(\s*\)", what="RT_CreateRenderResources")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].rstrip("\r\n") == "}"), None)
    if end is None:
        raise AnchorError("RT_CreateRenderResources: no closing brace")
    return end


def loc_thread_release(lines):
    # In the FRR_SYSTEM_RESOURCES branch, right after the Scaleform resources (#if ... SF_DestroyResources(); #endif).
    start = find_one(lines, r"^void\s+CD3D9Renderer::RT_ReleaseRenderResources\s*\(", what="RT_ReleaseRenderResources")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].rstrip("\r\n") == "}"), None)
    if end is None:
        raise AnchorError("RT_ReleaseRenderResources: no closing brace")
    branch = find_one(lines, r"if\s*\(\s*nFlags\s*&\s*FRR_SYSTEM_RESOURCES\s*\)", start, end, what="FRR_SYSTEM_RESOURCES branch")
    sf = find_one(lines, r"^\s*SF_DestroyResources\(\);", branch, end, what="SF_DestroyResources() anchor")
    if sf + 1 >= end or not re.match(r"^\s*#endif", lines[sf + 1]):
        raise AnchorError("SF_DestroyResources() anchor: the next line is not #endif")
    return sf + 2


def loc_cmake(lines):
    return find_one(lines, r"^end_sources\(\)", what="CMakeLists.txt end_sources() anchor")


HOOKS = [
    (RD + "DriverD3D.cpp", [
        ("include", "//", ['#include "../XUI2DL/XUI2DL.h"'], loc_driver_include, None),
        ("EndFrame", "//", ["XUI2DL::EndFrame(m_activeContextKey);"], loc_driver_endframe, "anchor"),
    ]),
    (RD + "D3DRenderThread.cpp", [
        ("include", "//", ['#include "../XUI2DL/XUI2DL.h"'], loc_thread_include, None),
        ("RT_ReleaseResources", "//", ["XUI2DL::RT_ReleaseResources();"], loc_thread_release, "\t\t"),
        ("RT_CreateResources", "//", ["XUI2DL::RT_CreateResources();"], loc_thread_create, "\t"),
    ]),
    (RD + "CMakeLists.txt", [
        ("sources", "#", [
            "add_sources(\"NoUberFile\"",
            "\tPROJECTS CryRenderGNM CryRenderD3D11 CryRenderD3D12 CryRenderVulkan",
            "\tSOURCE_GROUP \"XUI2DL\"",
            "\t\t\"../XUI2DL/XUI2DLApi.h\"",
            "\t\t\"../XUI2DL/XUI2DL.h\"",
            "\t\t\"../XUI2DL/XUI2DL.cpp\"",
            ")",
        ], loc_cmake, None),
    ]),
]


def read_text(path):
    with open(path, "rb") as f:
        data = f.read()
    text = data.decode("latin-1")   # byte-preserving
    nl = "\r\n" if "\r\n" in text else "\n"
    return text, nl


def split_lines(text):
    return text.splitlines(True)


def block_marks(prefix):
    return prefix + " XUI2DL begin", prefix + " XUI2DL end"


def find_blocks(lines, prefix):
    """[(begin index, end index inclusive)] of every marked block."""
    begin, end = block_marks(prefix)
    blocks, open_at = [], None
    for i, line in enumerate(lines):
        s = line.strip()
        if s == begin:
            if open_at is not None:
                raise AnchorError("nested %r at line %d" % (begin, i + 1))
            open_at = i
        elif s == end:
            if open_at is None:
                raise AnchorError("%r without begin at line %d" % (end, i + 1))
            blocks.append((open_at, i))
            open_at = None
    if open_at is not None:
        raise AnchorError("%r without end at line %d" % (begin, open_at + 1))
    return blocks


def block_contains(lines, block, body):
    inner = [l.strip() for l in lines[block[0] + 1: block[1]] if l.strip()]
    return inner == [b.strip() for b in body if b.strip()]


def plan_file(root, rel, hooks):
    """Returns (path, text, nl, [(hook name, present, insertion index, block lines)]); raises AnchorError."""
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        raise AnchorError("%s: file not found" % rel)
    text, nl = read_text(path)
    lines = split_lines(text)
    result = []
    for name, prefix, body, locate, indent in hooks:
        blocks = find_blocks(lines, prefix)
        present = any(block_contains(lines, b, body) for b in blocks)
        try:
            at = locate(lines)
        except AnchorError as e:
            if present:
                at = None   # applied already; the anchor may have moved with it
            else:
                raise AnchorError("%s: %s" % (rel, e))
        if indent == "anchor":
            ind = indent_of(lines[at]) if at is not None else ""
        else:
            ind = indent or ""
        begin, end = block_marks(prefix)
        block = [ind + begin] + [(ind + b) if b else "" for b in body] + [ind + end]
        result.append((name, present, at, block))
    return path, text, nl, result


def apply_insertions(text, nl, planned):
    lines = split_lines(text)
    # Insert from the bottom so earlier indices stay valid.
    for name, present, at, block in sorted((p for p in planned if not p[1]), key=lambda p: p[2], reverse=True):
        lines[at:at] = [l + nl for l in block]
    return "".join(lines)


def remove_blocks(text, prefix_set):
    lines = split_lines(text)
    removed = 0
    for prefix in prefix_set:
        for b0, b1 in reversed(find_blocks(lines, prefix)):
            del lines[b0: b1 + 1]
            removed += 1
    return "".join(lines), removed


def file_state(root, src_rel, dst_rel):
    src = os.path.join(HERE, src_rel)
    dst = os.path.join(root, dst_rel)
    if not os.path.isfile(dst):
        return "missing"
    with open(src, "rb") as a, open(dst, "rb") as b:
        return "same" if a.read() == b.read() else "different"


def write_bytes(path, text):
    with open(path, "wb") as f:
        f.write(text.encode("latin-1"))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = [a for a in argv[1:] if a.startswith("--")]
    if len(args) != 1 or any(f not in ("--check", "--remove") for f in flags) or len(flags) > 1:
        print(__doc__.strip())
        return 2
    root = os.path.abspath(args[0])
    mode = flags[0] if flags else "--apply"
    if not os.path.isdir(os.path.join(root, "Code", "CryEngine", "RenderDll")):
        print("error: %s is not an engine root (no Code/CryEngine/RenderDll)" % root)
        return 2
    for src_rel, _ in FILES:
        if not os.path.isfile(os.path.join(HERE, src_rel)):
            print("error: patch file missing: %s" % src_rel)
            return 2

    if mode == "--remove":
        results = []
        try:
            for rel, hooks in HOOKS:
                path = os.path.join(root, rel)
                if not os.path.isfile(path):
                    raise AnchorError("%s: file not found" % rel)
                text, _ = read_text(path)
                new_text, n = remove_blocks(text, sorted(set(h[1] for h in hooks)))
                results.append((rel, path, new_text, n, text))
        except AnchorError as e:
            print("error: %s (nothing written)" % e)
            return 2
        for rel, path, new_text, n, text in results:
            if n:
                write_bytes(path, new_text)
            print("%-45s %d block(s) removed" % (rel, n))
        print("copied files left in place")
        return 0

    # Plan everything first: an anchor error writes nothing.
    try:
        plans = [(rel,) + plan_file(root, rel, hooks) for rel, hooks in HOOKS]
    except AnchorError as e:
        print("error: %s (nothing written)" % e)
        return 2

    if mode == "--check":
        complete = True
        for src_rel, dst_rel in FILES:
            st = file_state(root, src_rel, dst_rel)
            complete = complete and st == "same"
            print("file  %-50s %s" % (dst_rel, st))
        for rel, path, text, nl, planned in plans:
            lines = split_lines(text)
            prefix = "#" if rel.endswith(".txt") else "//"
            for name, present, at, block in planned:
                complete = complete and present
                if present:
                    where = [b[0] + 1 for b in find_blocks(lines, prefix) if block_contains(lines, b, block[1:-1])]
                    print("hook  %-50s present (marker line %s)" % (rel + " " + name, ",".join(str(w) for w in where)))
                else:
                    print("hook  %-50s missing (would insert before line %d)" % (rel + " " + name, at + 1))
        print("state: %s" % ("applied" if complete else "NOT fully applied"))
        return 0 if complete else 1

    # Apply: copy the files, then insert the missing hooks.
    for src_rel, dst_rel in FILES:
        st = file_state(root, src_rel, dst_rel)
        if st != "same":
            dst = os.path.join(root, dst_rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(os.path.join(HERE, src_rel), "rb") as a, open(dst, "wb") as b:
                b.write(a.read())
        print("file  %-50s %s" % (dst_rel, "unchanged" if st == "same" else ("copied" if st == "missing" else "updated")))
    for rel, path, text, nl, planned in plans:
        todo = [p for p in planned if not p[1]]
        if todo:
            write_bytes(path, apply_insertions(text, nl, planned))
        new_lines = split_lines(read_text(path)[0])
        for name, present, at, block in planned:
            print("hook  %-50s %s" % (rel + " " + name, "already present" if present else "inserted"))
        if todo:
            print("      %s marker line(s): %s" % (rel, ",".join(str(i + 1) for i, l in enumerate(new_lines) if "XUI2DL begin" in l)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
