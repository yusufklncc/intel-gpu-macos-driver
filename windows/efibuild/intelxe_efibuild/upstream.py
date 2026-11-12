# SPDX-License-Identifier: MIT
"""OpCore Simplify and Hardware Sniffer at the commits pinned in upstream/PINNED.json.

They are not in this repository. The source archive of the pinned commit is downloaded from GitHub and unpacked into the
cache; every file is then checked against upstream/<project>.sha256 (one "sha256  path" line per file, the whole list
also pinned by its own SHA-256 in PINNED.json). The archive's own bytes are not pinned because GitHub does not promise
stable archive bytes; the files inside are what is checked, every time a build starts. Missing, changed or extra files
stop the build. A release of the Windows app ships the checked tree with its LICENSE; it does not fetch code at run time.

  python -I efibuild.py upstream --fetch            download and check both projects
  python -I efibuild.py upstream --manifest NAME    (maintainer) write the manifest of a freshly fetched tree
"""
import io
import os
import shutil
import tarfile
import tempfile

from . import pins

UPSTREAM_DIR = os.path.join(pins.HERE, "upstream")


def pinned():
    data = pins.load_json(pins.PINNED_PATH)
    if data.get("format") != "intelxe-efibuild-upstream" or data.get("format_version") != 1:
        raise pins.PinError("upstream/PINNED.json: unknown format")
    return data["projects"]


def manifest_text(tree):
    """The manifest of a tree: "sha256  relative/path" per regular file, sorted, LF line ends."""
    lines = []
    for root, dirs, files in os.walk(tree):
        dirs[:] = sorted(d for d in dirs if d not in ("__pycache__",))
        for name in files:
            path = os.path.join(root, name)
            if os.path.islink(path):
                raise pins.PinError("link in the upstream tree: " + path)
            rel = os.path.relpath(path, tree).replace(os.sep, "/")
            lines.append("{}  {}".format(pins.sha256_file(path), rel))
    return "".join(line + "\n" for line in sorted(lines, key=lambda x: x[66:]))


def _extract(archive_bytes, dest):
    """Unpack a GitHub source tarball into dest, dropping its top folder. Regular files and folders only."""
    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:gz") as tar:
        for member in tar.getmembers():
            parts = member.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            rel = parts[1]
            if rel.startswith("/") or ".." in rel.split("/"):
                raise pins.PinError("unsafe path in the archive: " + member.name)
            target = os.path.join(dest, *rel.split("/"))
            if member.isdir():
                os.makedirs(target, exist_ok=True)
            elif member.isfile():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with tar.extractfile(member) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out)
            elif member.type == tarfile.XGLTYPE or member.type == tarfile.XHDTYPE or member.name.endswith("pax_global_header"):
                continue
            else:
                raise pins.PinError("the archive holds a link or device: " + member.name)


def tree_path(name, cache_root=None):
    proj = pinned()[name]
    return os.path.join(cache_root or pins.default_cache(), "upstream", "{}-{}".format(name, proj["commit"]))


def verify(name, tree):
    """Raise unless tree holds exactly the pinned files of project name."""
    proj = pinned()[name]
    manifest_path = os.path.join(UPSTREAM_DIR, proj["manifest"])
    with open(manifest_path, "rb") as f:
        expected = f.read()
    if pins.sha256_bytes(expected) != proj["manifest_sha256"]:
        raise pins.PinError("{} does not match manifest_sha256 in PINNED.json".format(proj["manifest"]))
    got = manifest_text(tree).encode()
    if got != expected:
        want = set(expected.decode().splitlines())
        have = set(got.decode().splitlines())
        diff = sorted(x[66:] for x in want ^ have)
        raise pins.PinError("{} at {} differs from its pinned manifest: {}".format(name, tree, ", ".join(diff[:10])))
    return tree


def fetch(name, cache_root=None, check=True):
    """The checked tree of project name, downloading it when the cache has none."""
    proj = pinned()[name]
    dest = tree_path(name, cache_root)
    if os.path.isdir(dest):
        if not check:
            return dest
        try:
            return verify(name, dest)
        except pins.PinError:
            shutil.rmtree(dest)
    data = pins.http_get(proj["archive"])
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix=".upstream-", dir=os.path.dirname(dest))
    try:
        _extract(data, tmp)
        if check:
            verify(name, tmp)
        os.replace(tmp, dest)
    finally:
        if os.path.isdir(tmp):
            shutil.rmtree(tmp, ignore_errors=True)
    return dest


def copy_for_build(name, work_dir, cache_root=None):
    """A fresh copy of the checked tree for one build (the engine writes into its own folder: macserial, caches)."""
    src = verify(name, fetch(name, cache_root))
    dst = os.path.join(work_dir, name)
    shutil.copytree(src, dst)
    return dst


def write_manifest(name, cache_root=None):
    """Maintainer step after changing a commit in PINNED.json: fetch unchecked, write the manifest, print its digest."""
    tree = fetch(name, cache_root, check=False)
    text = manifest_text(tree)
    proj = pinned()[name]
    path = os.path.join(UPSTREAM_DIR, proj["manifest"])
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return pins.sha256_bytes(text.encode()), text.count("\n")
