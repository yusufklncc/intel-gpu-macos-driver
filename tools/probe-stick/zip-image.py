#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# zip-image.py: the Xe probe stick's image as a release ships it, xe-probe-stick-VERSION.img.zip, and its check.
#
#   python3 -I tools/probe-stick/zip-image.py check IMAGE [--version V]       the image (as build.sh wrote it) is fit to ship
#   python3 -I tools/probe-stick/zip-image.py zip IMAGE OUT_DIR [--version V]  writes OUT_DIR/xe-probe-stick-V.img.zip
#   python3 -I tools/probe-stick/zip-image.py verify ZIP [--version V]         a zip as release.sh checks it before publishing
#
# The image must be named xe-probe-stick-V.img (V: --version when given, tools/probe-stick/VERSION in release/package.sh),
# have its .img.sha256 beside it (build.sh writes both) naming it with the right SHA-256, and have the stick's layout: a
# whole number of 512-byte sectors, an MBR whose first partition is FAT32 and holds a FAT32 boot sector labelled
# XEPROBE (the same check as IntelXe Manager's ProbeImage.CheckLayout). The zip holds exactly the image and its
# .img.sha256, at its top level, with fixed dates (the same image gives the same zip). The release lists the zip in its
# signed list of hashes; the .img.sha256 rides inside it, so the release has no separate .sha256 asset for the image
# (IntelXe Manager for macOS finds a release's list of hashes by the ending .sha256). IntelXe Manager for Windows
# downloads the zip, checks it against the signed list and unpacks the two files (ProbeStick/ProbeDownload.cs).
import hashlib, os, re, struct, sys, zipfile

NAME = re.compile(r"^xe-probe-stick-(\d+(?:\.\d+){0,3})\.img$")
MAX = 4 << 30
DATE = (1980, 1, 1, 0, 0, 0)


def fail(m):
    print("error: " + m, file=sys.stderr)
    sys.exit(1)


def layout(f, length, what):
    """The stick's layout, as ProbeImage.CheckLayout reads it."""
    if length < (1 << 20) or length > MAX: fail(f"{what}: {length} bytes, not a probe stick image (about 200 MB)")
    if length % 512: fail(f"{what}: its size is not a whole number of 512-byte sectors")
    f.seek(0)
    mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa": fail(f"{what}: no MBR")
    ptype = mbr[446 + 4]
    start = struct.unpack_from("<I", mbr, 446 + 8)[0]
    if ptype not in (0x0B, 0x0C): fail(f"{what}: its first partition is not FAT32 (type 0x{ptype:02X})")
    if start == 0 or (start + 1) * 512 > length: fail(f"{what}: its first partition starts outside the image")
    f.seek(start * 512)
    boot = f.read(512)
    if boot[510:512] != b"\x55\xaa" or boot[82:90] != b"FAT32   ": fail(f"{what}: its partition holds no FAT32 file system")
    label = boot[71:82].decode("ascii", "replace").rstrip()
    if label != "XEPROBE": fail(f"{what}: its partition is labelled {label!r}, not XEPROBE")


def sha_line(text, name, what):
    """The SHA-256 a one-line sha256sum file gives for name."""
    fields = text.split()
    if len(fields) < 2 or not re.fullmatch(r"[0-9a-fA-F]{64}", fields[0]) or fields[1].lstrip("*") != name:
        fail(f"{what} holds no SHA-256 line for {name} (\"HASH  {name}\", as build.sh writes it)")
    return fields[0].lower()


def check(img, version):
    name = os.path.basename(img)
    m = NAME.match(name)
    if not m: fail(f"{img}: not named xe-probe-stick-VERSION.img (as build.sh names it)")
    if version and m.group(1) != version: fail(f"{name} is version {m.group(1)}, not {version} (tools/probe-stick/VERSION)")
    if not os.path.isfile(img): fail(f"no file {img}")
    if not os.path.isfile(img + ".sha256"): fail(f"no {name}.sha256 beside {img} (build.sh writes it)")
    with open(img + ".sha256", encoding="utf-8") as s:
        text = s.read(4097)
    if len(text) > 4096: fail(f"{name}.sha256 is not a SHA-256 file")
    want = sha_line(text, name, name + ".sha256")
    h = hashlib.sha256()
    with open(img, "rb") as f:
        length = os.fstat(f.fileno()).st_size
        layout(f, length, name)
        f.seek(0)
        for b in iter(lambda: f.read(4 << 20), b""): h.update(b)
    if h.hexdigest() != want: fail(f"{name}: SHA-256 {h.hexdigest()}, not its .img.sha256's {want} (damaged, or another build)")
    return name, text, length


def info(name):
    zi = zipfile.ZipInfo(name, DATE)
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    zi.create_system = 3
    return zi


def make(img, out, version):
    name, text, length = check(img, version)
    os.makedirs(out, exist_ok=True)
    z = os.path.join(out, name + ".zip")
    tmp = z + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as zf:
        with open(img, "rb") as f, zf.open(info(name), "w", force_zip64=length >= (1 << 31)) as w:
            for b in iter(lambda: f.read(4 << 20), b""): w.write(b)
        zf.writestr(info(name + ".sha256"), text.encode("utf-8"))
    os.replace(tmp, z)
    verify(z, version)


def verify(z, version):
    zname = os.path.basename(z)
    if not zname.endswith(".img.zip"): fail(f"{z}: not an .img.zip")
    name = zname[:-4]
    m = NAME.match(name)
    if not m: fail(f"{zname}: not named xe-probe-stick-VERSION.img.zip")
    if version and m.group(1) != version: fail(f"{zname} is version {m.group(1)}, not {version} (tools/probe-stick/VERSION)")
    try:
        zf = zipfile.ZipFile(z)
        bad = zf.testzip()
    except Exception as e:
        fail(f"{zname}: not a zip ({e})")
    if bad: fail(f"{zname}: damaged entry {bad}")
    names = sorted(i.filename for i in zf.infolist())
    if names != sorted([name, name + ".sha256"]): fail(f"{zname} holds {names}, not exactly {name} and {name}.sha256")
    want = sha_line(zf.read(name + ".sha256").decode("utf-8"), name, name + ".sha256 in " + zname)
    entry = zf.getinfo(name)
    h = hashlib.sha256()
    with zf.open(name) as f:
        for b in iter(lambda: f.read(4 << 20), b""): h.update(b)
    if h.hexdigest() != want: fail(f"{name} in {zname} does not match its .img.sha256")
    with zf.open(name) as f:   # a ZipExtFile seeks (forward by reading)
        layout(f, entry.file_size, name + " in " + zname)
    print(f"  ok    {zname}: {name} ({entry.file_size / 1e6:.0f} MB, {os.path.getsize(z) / 1e6:.0f} MB zipped) and its .img.sha256, they match")


def main(a):
    version = None
    if "--version" in a:
        i = a.index("--version")
        if i + 1 >= len(a): fail("--version needs a value")
        version = a[i + 1]
        del a[i:i + 2]
    if len(a) == 2 and a[0] == "check":
        name, _, length = check(a[1], version)
        print(f"  ok    {name}: {length / 1e6:.0f} MB, the stick's layout, its .img.sha256 matches")
    elif len(a) == 3 and a[0] == "zip": make(a[1], a[2], version)
    elif len(a) == 2 and a[0] == "verify": verify(a[1], version)
    else:
        sys.stderr.write("".join(l[2:] for l in open(__file__, encoding="utf-8").readlines()[3:8]))
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv[1:])
