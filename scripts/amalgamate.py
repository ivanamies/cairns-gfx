#!/usr/bin/env python3
"""Amalgamate the cairns-gfx source into one text blob.

Concatenates the engine source, shaders, and build files into a single stream
(default: stdout) with per-file delimiters, so the whole project can be pasted
into an LLM in one shot. Skips third_party/, build dirs, .git, and media.

Usage:
    python3 scripts/amalgamate.py              # write to stdout
    python3 scripts/amalgamate.py out.txt      # write to a file
"""
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

INCLUDE_EXT = {
    ".hpp", ".h", ".cpp", ".cc", ".mm", ".c",
    ".vert", ".frag", ".comp", ".metal", ".glsl",
}
INCLUDE_NAMES = {"CMakeLists.txt"}
SKIP_DIRS = {".git", "third_party", "scripts"}


def should_skip_dir(name):
    return name in SKIP_DIRS or name.startswith("build") or name.startswith(".")


def collect(root):
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not should_skip_dir(d)]
        for fn in filenames:
            if os.path.splitext(fn)[1] in INCLUDE_EXT or fn in INCLUDE_NAMES:
                found.append(os.path.join(dirpath, fn))
    found.sort()
    return found


def main():
    out = open(sys.argv[1], "w") if len(sys.argv) > 1 else sys.stdout
    files = collect(REPO_ROOT)
    for path in files:
        rel = os.path.relpath(path, REPO_ROOT)
        out.write("\n// ==================== {} ====================\n".format(rel))
        with open(path, "r", errors="replace") as f:
            out.write(f.read())
    if out is not sys.stdout:
        out.close()
        sys.stderr.write("amalgamated {} files -> {}\n".format(len(files), sys.argv[1]))


if __name__ == "__main__":
    main()
