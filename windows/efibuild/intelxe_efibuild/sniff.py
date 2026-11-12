# SPDX-License-Identifier: MIT
"""Hardware Sniffer on Windows: this PC's Report.json and ACPI tables, for the build.

Hardware Sniffer (BSD-3-Clause, the commit pinned in upstream/PINNED.json) reads the hardware through WMI (it needs the
`wmi` package and pywin32 in the Python that runs it) and dumps the ACPI tables with acpidump.exe. Its own code
downloads acpidump.exe from ACPICA's releases without a checksum; here acpidump.exe is the one pinned in
downloads.lock.json ("windows_tools", ACPICA 20260408, checked by SHA-256) and nothing else is downloaded. Dumping the
tables needs administrator rights.

Of the dump, only the tables OpCore Simplify reads (DSDT, SSDT, APIC, DMAR) are kept: the others can hold data the build
has no use for (MSDM holds the Windows product key).

  efibuild.py sniff --out DIR      DIR/Report.json and DIR/ACPI/
"""
import os
import shutil
import sys
import tempfile

from . import pins
from . import upstream

KEEP_TABLES = (b"DSDT", b"SSDT", b"APIC", b"DMAR")


class SniffError(RuntimeError):
    pass


def windows_tool(name, lock=None, cache=None):
    """iasl.exe or acpidump.exe from the pinned ACPICA release, checked before it is returned."""
    lock = lock or pins.Lock()
    cache = cache or pins.Cache()
    entry = (lock.data.get("windows_tools") or {}).get(name)
    if not entry:
        raise pins.PinError("downloads.lock.json pins no " + name)
    src = cache.fetch(entry["url"], entry["sha256"])
    out_dir = os.path.join(cache.root, "bin")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, entry["sha256"][:16] + "-" + name)
    if not os.path.isfile(out) or pins.sha256_file(out) != entry["sha256"]:
        pins.copy_verified(src, out, entry["sha256"])
    if pins.sha256_file(out) != entry["sha256"]:
        raise pins.PinError(out + " changed")
    return out


def keep_build_tables(src_dir, dst_dir):
    """Copy the tables the build reads (by signature, not by name); returns their names."""
    os.makedirs(dst_dir, exist_ok=True)
    kept = []
    for name in sorted(os.listdir(src_dir)):
        path = os.path.join(src_dir, name)
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        with open(path, "rb") as f:
            sig = f.read(4)
        if sig in KEEP_TABLES:
            shutil.copyfile(path, os.path.join(dst_dir, name))
            kept.append(name)
    return kept


def sniff(out_dir, cache_root=None):
    if os.name != "nt":
        raise SniffError("Hardware Sniffer reads the hardware of the Windows PC it runs on; on this system pass "
                         "--report and --acpi from a Windows run")
    cache = pins.Cache(cache_root)
    acpidump = windows_tool("acpidump.exe", cache=cache)
    work = tempfile.mkdtemp(prefix="sniff-", dir=cache.root)
    hs = upstream.copy_for_build("Hardware-Sniffer", work, cache_root)
    raw = os.path.join(work, "SysReport")
    sys.path.insert(0, hs)
    try:
        import HardwareSniffer  # noqa: PLC0415 - the pinned upstream module
        HardwareSniffer.HardwareSniffer.check_acpidump = lambda self_: acpidump
        from Scripts import utils as hs_utils  # noqa: PLC0415
        hs_utils.Utils.head = lambda self_, text=None, width=68, resize=True: print("== {}".format(text or ""))
        h = HardwareSniffer.HardwareSniffer(raw, rich_format=False)
        h.hardware_info.hardware_collector()
        h.export_hardware_report()
        h.dump_acpi_tables()
    finally:
        sys.path.remove(hs)
    report = os.path.join(raw, "Report.json")
    if not os.path.isfile(report):
        raise SniffError("Hardware Sniffer wrote no Report.json")
    os.makedirs(out_dir, exist_ok=True)
    shutil.copyfile(report, os.path.join(out_dir, "Report.json"))
    kept = keep_build_tables(os.path.join(raw, "ACPI"), os.path.join(out_dir, "ACPI"))
    shutil.rmtree(work, ignore_errors=True)
    if not any(n.lower().startswith("dsdt") for n in kept):
        raise SniffError("no DSDT in the ACPI dump (run as administrator)")
    return os.path.join(out_dir, "Report.json"), os.path.join(out_dir, "ACPI"), kept
