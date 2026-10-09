#!/usr/bin/env python3
"""Packages the sco SDK into sco-sdk-<version>.zip.

    sdk/package.py [--out DIR]          # writes DIR/sco-sdk-<version>.zip (default: sdk/out)

The zip holds one folder, sco-sdk-<version>/, with everything a modder needs and nothing else:
sco_api.h, sco_storage.h and the C++20 headers (include/scosdk/), the CMake helper, the template, the examples, sco-plugin-check, the docs, LICENSE
and SHA256SUMS. It needs only Python 3 (no zip tool), and the same sources always give the same
bytes: entries are sorted and carry a fixed timestamp.

Run by .github/workflows/sdk.yml; CI then builds the examples from the unpacked zip alone.
"""
import argparse
import hashlib
import os
import re
import sys
import zipfile

SDK = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SDK)
REPO_URL = "https://github.com/scubamount/sco-core/blob/main/"
FIXED_TIME = (2026, 1, 1, 0, 0, 0)

# (source relative to the repo root, path inside the SDK folder). Folders are copied whole.
CONTENT = [
    ("include/sco_api.h", "include/sco_api.h"),
    ("include/sco_storage.h", "include/sco_storage.h"),
    ("include/scosdk", "include/scosdk"),
    ("LICENSE", "LICENSE"),
    ("sdk/VERSION", "VERSION"),
    ("sdk/README.md", "README.md"),
    ("sdk/CMakeLists.txt", "CMakeLists.txt"),
    ("sdk/cmake", "cmake"),
    ("sdk/template", "template"),
    ("sdk/examples", "examples"),
    ("sdk/tools", "tools"),
    ("sdk/docs", "docs"),
    ("docs/api-v1.md", "docs/api-v1.md"),
    ("docs/sdk-cpp.md", "docs/sdk-cpp.md"),
]
SKIP_DIRS = {"build", "out", ".DS_Store"}


def files_under(src):
    if os.path.isfile(src):
        yield src, ""
        return
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if name in SKIP_DIRS:
                continue
            full = os.path.join(dirpath, name)
            yield full, os.path.relpath(full, src).replace(os.sep, "/")


LINK = re.compile(r"\]\(([^)#\s]+)((?:#[^)]*)?)\)")


def to_zip_path(repo_path):
    """The path a repository file has inside the SDK folder, or None if it isn't shipped."""
    for src_rel, dst_rel in CONTENT:
        if repo_path == src_rel:
            return dst_rel
        if repo_path.startswith(src_rel + "/") and os.path.isdir(os.path.join(ROOT, src_rel)):
            return dst_rel + repo_path[len(src_rel):]
    return None


def fix_links(src_full, dst, data):
    """Markdown links are written to work in the repository. In the zip, a link to a shipped
    file points at its zip path; a link to anything else points at GitHub."""
    if not dst.endswith(".md"):
        return data
    src_dir = os.path.dirname(os.path.relpath(src_full, ROOT))
    dst_dir = os.path.dirname(dst)

    def repl(m):
        target, anchor = m.group(1), m.group(2)
        if re.match(r"[a-z]+:", target):
            return m.group(0)
        repo_path = os.path.normpath(os.path.join(src_dir, target)).replace(os.sep, "/")
        zip_path = to_zip_path(repo_path)
        if zip_path is None:
            return f"]({REPO_URL}{repo_path}{anchor})"
        return f"]({os.path.relpath(zip_path, dst_dir or '.').replace(os.sep, '/')}{anchor})"

    return LINK.sub(repl, data.decode("utf-8")).encode("utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(SDK, "out"))
    args = ap.parse_args()

    with open(os.path.join(SDK, "VERSION"), encoding="utf-8") as f:
        version = f.read().strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(-[a-z0-9.]+)?", version):
        sys.exit(f"sdk/VERSION: '{version}' is not a version")
    top = f"sco-sdk-{version}"

    entries = {}
    for src_rel, dst_rel in CONTENT:
        src = os.path.join(ROOT, src_rel)
        if not os.path.exists(src):
            sys.exit(f"missing {src_rel}")
        for full, sub in files_under(src):
            dst = dst_rel if not sub else f"{dst_rel}/{sub}"
            if dst in entries:
                sys.exit(f"two sources for {dst}")
            with open(full, "rb") as f:
                entries[dst] = (fix_links(full, dst, f.read()), os.access(full, os.X_OK))

    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n"
                   for name, (data, _) in sorted(entries.items()))
    entries["SHA256SUMS"] = (sums.encode("ascii"), False)

    os.makedirs(args.out, exist_ok=True)
    zip_path = os.path.join(args.out, f"{top}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(entries):
            data, executable = entries[name]
            info = zipfile.ZipInfo(f"{top}/{name}", FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3  # unix, so the mode bits below are honored
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            z.writestr(info, data)

    with open(zip_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    print(f"{zip_path}: {len(entries)} files, sha256 {digest}")


if __name__ == "__main__":
    main()
