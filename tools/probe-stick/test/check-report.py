#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Judge the two QEMU boots of qemu-test.sh: check-report.py TESTDIR (the planted values in $SECRETS).

Boot 1 is a clean scan; boot 2 hangs in collect-after on purpose (xestick.testhang) until its dead-man powers the PC
off; boot 3 must leave that step out (and ignore a freeze planted for another computer); boot 4 is the safe scan.
Prints one line per check and exits 1 if any fails."""
import gzip
import hashlib
import os
import re
import sys
import tarfile

T = sys.argv[1]
SECRETS = os.environ["SECRETS"].split()
fails = []


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + ("   [%s]" % detail if detail != "" else ""))
    if not cond:
        fails.append(name)


def forms(v):
    """Every spelling of a planted value that would identify it."""
    out = {v, v.upper(), v.lower()}
    if re.fullmatch(r"([0-9a-f]{2}:){5}[0-9a-f]{2}", v):
        out |= {v.replace(":", ""), v.replace(":", "").upper(), v.replace(":", "-"), v.replace(":", "-").upper()}
    return out


def members(tf):
    """name -> bytes for every file in the report, trace .gz files unpacked too."""
    out = {}
    for m in tf.getmembers():
        if m.isfile():
            data = tf.extractfile(m).read()
            out[m.name] = data
            if m.name.endswith(".gz"):
                out[m.name + " (unpacked)"] = gzip.decompress(data)
    return out


def judge(n, previous=(), safe=False):
    print("\n== boot %d" % n)
    rc, secs = open(os.path.join(T, "qemu-%d.rc" % n)).read().split()
    check("QEMU powered off by itself (no time-out)", rc == "0", "exit %s after %s s" % (rc, secs))
    serial = open(os.path.join(T, "serial-%d.log" % n), "rb").read().decode("utf-8", "replace")
    check("the scan said Done on the console", "Done. On the stick: REPORTS/" in serial,
          "\n".join(serial.splitlines()[-15:]) if "Done. On the stick" not in serial else "")
    d = os.path.join(T, "reports-%d" % n)
    files = sorted(os.listdir(d)) if os.path.isdir(d) else []
    tars = [f for f in files if f.endswith(".tar.gz") and f not in previous]
    check("exactly one new report on the stick", len(tars) == 1, files)
    check("no half-written file and no privacy-check failure", not [f for f in files if f.endswith(".part") or "privacy" in f], files)
    a = open(os.path.join(T, "disk-before-%d.sha256" % n)).read().split()[0]
    b = open(os.path.join(T, "disk-after-%d.sha256" % n)).read().split()[0]
    check("the SATA disk is byte-identical after the scan", a == b, a[:12] + " -> " + b[:12])
    if len(tars) != 1:
        return None, files
    base = tars[0][:-len(".tar.gz")]
    for ext in (".txt", ".sha256"):
        check("%s%s sits next to it" % (base, ext), os.path.exists(os.path.join(d, base + ext)))
    seal = {}
    for line in open(os.path.join(d, base + ".sha256")):
        h, f = line.split()
        seal[f] = h
    ok = all(hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest() == h for f, h in seal.items())
    check("the .sha256 seal verifies the .tar.gz and the .txt", ok and set(seal) == {base + ".tar.gz", base + ".txt"}, seal)
    tf = tarfile.open(os.path.join(d, base + ".tar.gz"))
    m = members(tf)
    root = base
    inner = {}
    for line in m.get(root + "/SEAL.sha256", b"").decode().splitlines():
        h, f = line.split(None, 1)
        inner[f.strip()] = h
    real = {k[len(root) + 1:]: hashlib.sha256(v).hexdigest() for k, v in m.items()
            if not k.endswith(" (unpacked)") and k != root + "/SEAL.sha256"}
    check("SEAL.sha256 lists every file in the report with its SHA-256", inner == real and len(real) > 20,
          sorted(set(inner) ^ set(real))[:5] or len(real))
    size = os.path.getsize(os.path.join(d, base + ".tar.gz"))
    check("the report fits a GitHub attachment (< 25 MB)", size < 25e6, "%.2f MB" % (size / 1e6))

    blob = b"\n".join(m.values()) + open(os.path.join(d, base + ".txt"), "rb").read() + \
        open(os.path.join(d, "xe-stick-state.txt"), "rb").read()
    for s in SECRETS:
        hits = sum(blob.count(f.encode()) for f in forms(s))
        check("planted %s appears nowhere in the report, its summary or the state file" % s, hits == 0, hits)
    get = lambda p: m.get(root + "/" + p, b"").decode("utf-8", "replace")
    red = get("stick/redaction.txt")
    nser = int((re.search(r"serial: (\d+)", red) or [0, 0])[1])
    check("sanity: the redaction learned the planted serials (DMI, disk, USB stick)", nser >= 4, red.splitlines()[:6])
    dm = get("stick/dmesg.txt")
    check("sanity: the kernel log saw the stick's serial and it was replaced",
          "SerialNumber: <redacted>" in dm or "<serial-redacted>" in dm, [l for l in dm.splitlines() if "SerialNumber" in l][:2])
    check("sanity: the report still names the machine and board (only serials removed)",
          "QEMU-XS" in get("before/system/dmi.txt") and "XS-BOARD" in get("before/system/dmi.txt"))

    net = get("stick/net.txt")
    rows = [l.split()[1:] for l in net.splitlines() if l.strip()]
    check("no network interface but lo, at every check", len(rows) >= 2 and all(r == ["lo"] for r in rows), net.strip())
    check("sanity: the e1000e network card is on the PCI bus (no interface means no driver)",
          "8086:10d3" in get("before/gpu/pci.txt") or "8086:10d3" in dm, "")
    locks = get("stick/locks.txt")
    check("the SATA disk and its decoy were set read-only", re.search(r"\bsda\s+read-only", locks) is not None, locks.strip())
    mounts = [l.split() for l in get("stick/mounts.txt").splitlines()]
    blk = [l for l in mounts if l and l[0].startswith("/dev/")]
    check("exactly one block device was mounted: the stick, at /mnt/stick",
          len(blk) == 1 and blk[0][1] == "/mnt/stick" and not blk[0][0].startswith("/dev/sda"), blk)
    summ = get("summary.txt")
    if not safe:
        check("the trace step skipped cleanly: no Intel display device",
              "trace: skipped: no Intel display-class PCI device" in summ, [l for l in summ.splitlines() if "trace" in l][:3])
    check("   ... no trace file was written", not [k for k in m if "/trace/" in k and not k.endswith("/")])
    mods = get("stick/modules.txt")
    check("   ... and no GPU driver was loaded", not re.search(r"^(xe|i915)\s", mods, re.M))
    check("the report has the summary of the collect-linux.sh run(s)", "-------- collect-linux.sh" in summ)
    check("before/ is there (collect-linux.sh without a GPU driver)", root + "/before/summary.txt" in m)
    return (m, summ, get, files)


key = open(os.path.join(T, "key.txt")).read().strip()
r1 = judge(1)
if r1[0] is not None:
    m1, summ1, get1, files1 = r1
    check("boot 1: after/ is there (a clean run does both collections)", "Steps\n  trace: skipped" in summ1 and
          any(k.endswith("/after/summary.txt") for k in m1))
    check("boot 1: no step was left out", "left out on this computer" not in summ1, "")
    files1 = r1[3]
else:
    files1 = r1[1]

print("\n== boot 2 (xestick.testhang=collect-after)")
rc, secs = open(os.path.join(T, "qemu-2.rc")).read().split()
serial2 = open(os.path.join(T, "serial-2.log"), "rb").read().decode("utf-8", "replace")
check("the scan hung in collect-after as told", "hanging here until the dead-man" in serial2)
check("   ... and the dead-man turned the PC off (QEMU exited by itself)",
      rc == "0" and "did not finish within 15 s" in serial2, "exit %s after %s s" % (rc, secs))
files2 = sorted(os.listdir(os.path.join(T, "reports-2")))
check("   ... no report was written", [f for f in files2 if f.endswith(".tar.gz")] == [f for f in files1 if f.endswith(".tar.gz")], files2)
st2 = open(os.path.join(T, "reports-2", "xe-stick-state.txt")).read()
check("   ... the step is still marked running on the stick", "running %s collect-after" % key in st2, st2)
a = open(os.path.join(T, "disk-before-2.sha256")).read().split()[0]
b = open(os.path.join(T, "disk-after-2.sha256")).read().split()[0]
check("   ... the SATA disk is byte-identical", a == b)

prev = [f for f in files2 if f.endswith(".tar.gz")]
r3 = judge(3, prev)
if r3[0] is not None:
    m3, summ3, get3, files3 = r3
    check("boot 3: the freeze of boot 2 is reported", "the last run stopped during: collect-after" in summ3,
          [l for l in summ3.splitlines() if "last run" in l])
    check("   ... collect-after was left out and says so", "collect-after: skipped" in summ3 and
          not any(k.endswith("/after/summary.txt") for k in m3))
    check("   ... collect-before still ran (another computer's freeze changes nothing)",
          any(k.endswith("/before/summary.txt") for k in m3))
    st = open(os.path.join(T, "reports-3", "xe-stick-state.txt")).read()
    check("   ... the state file now says skip for this computer", "skip %s collect-after" % key in st, st)
    check("   ... no step of this computer is left 'running'", "running %s " % key not in st, st)
    check("   ... the other computer's line is untouched", "running 000000000000 collect-before" in st, st)
    check("   ... the machine key in the report matches", key in get3("stick/stages.txt"), get3("stick/stages.txt"))

prev = [f for f in (r3[1] if r3[0] is None else r3[3]) if f.endswith(".tar.gz")]
r4 = judge(4, prev, safe=True)
if r4[0] is not None:
    m4, summ4, get4, files4 = r4
    check("boot 4 (safe scan): mode safe, nomodeset on the command line", "mode safe" in summ4 and
          "nomodeset" in get4("stick/cmdline.txt"))
    check("   ... no GPU step ran and no after/ collection", "safe mode: no GPU driver was started" in summ4 and
          not any(k.endswith("/after/summary.txt") for k in m4) and "trace: skipped" not in summ4)

print("\n%s" % ("ALL PASSED" if not fails else "%d FAILED: %s" % (len(fails), "; ".join(fails))))
sys.exit(1 if fails else 0)
