# SPDX-License-Identifier: MIT
"""Checks of a built EFI, and the pinned OpenCore tools.

- ocvalidate from the OpenCore release the EFI was built from (the OpenCorePkg zip pinned in downloads.lock.json; the
  binary is taken out of that zip and checked against its own pinned SHA-256 before every run). A validator that
  cannot run is a failure, never a pass.
- the EFI's OpenCore files are that release's, byte for byte;
- stage 2 complete (efi-rules.json "settings", and "safari" unless it was left out);
- stage 4 not yet and possible: no IntelXe.kext or SSDT-ARCOFF.aml, nothing that turns the card off or binds a driver
  to it, no WhateverGreen, no block of the firmware display driver; every entry's file present;
- SMBIOS values (serial, MLB, UUID, ROM) never appear in what the builder prints or writes besides config.plist.
"""
import base64
import os
import platform
import plistlib
import re
import stat
import subprocess
import zipfile
import io

from . import pins
from . import rules as rules_mod

SECRET_KEYS = ("SystemSerialNumber", "MLB", "SystemUUID", "ROM")


class ValidatorUnavailable(RuntimeError):
    pass


# ---- the pinned tools ------------------------------------------------------------------------------------------------

def host_os():
    return {"Darwin": "Darwin", "Windows": "Windows", "Linux": "Linux"}.get(platform.system(), platform.system())


class Tools:
    """ocvalidate and macserial out of the pinned OpenCorePkg zip, each checked against the lock."""

    def __init__(self, lock, cache):
        self.lock, self.cache = lock, cache

    def zip_bytes(self):
        oc = self.lock.products.get("OpenCorePkg")
        if not oc:
            raise ValidatorUnavailable("downloads.lock.json pins no OpenCorePkg")
        return self.cache.read(oc["url"], oc["sha256"])

    def path(self, name):
        entry = (self.lock.tools.get(name) or {}).get(host_os())
        if not entry:
            raise ValidatorUnavailable("downloads.lock.json pins no {} for {}".format(name, host_os()))
        with zipfile.ZipFile(io.BytesIO(self.zip_bytes())) as z:
            data = z.read(entry["member"])
        if pins.sha256_bytes(data) != entry["sha256"]:
            raise ValidatorUnavailable("{} in the OpenCore zip is not the pinned one".format(entry["member"]))
        out_dir = os.path.join(self.cache.root, "bin")
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, entry["sha256"][:16] + "-" + os.path.basename(entry["member"]))
        if os.path.islink(out):
            raise ValidatorUnavailable(out + " is a link")
        if not os.path.isfile(out) or pins.sha256_file(out) != entry["sha256"]:
            tmp = out + ".tmp"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, out)
        os.chmod(out, os.stat(out).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        if pins.sha256_file(out) != entry["sha256"]:   # again, right before it is run
            raise ValidatorUnavailable(out + " changed")
        return out

    def release_file(self, member):
        with zipfile.ZipFile(io.BytesIO(self.zip_bytes())) as z:
            try:
                return z.read(member)
            except KeyError:
                return None


# ---- secrets ---------------------------------------------------------------------------------------------------------

def read_secrets(config_path):
    """The identity OpCore Simplify wrote with macserial: every form it could be printed in."""
    try:
        with open(config_path, "rb") as f:
            generic = plistlib.load(f)["PlatformInfo"]["Generic"]
    except (OSError, KeyError, plistlib.InvalidFileException):
        return []
    out = []
    for key in SECRET_KEYS:
        v = generic.get(key)
        if isinstance(v, (bytes, bytearray)):
            v = bytes(v)
            if v:
                out += [v.hex(), v.hex().upper(), base64.b64encode(v).decode(), ":".join("%02x" % b for b in v),
                        ":".join("%02X" % b for b in v)]
        elif isinstance(v, str) and len(v) >= 6:
            out += [v, v.lower(), v.upper()]
    return sorted(set(x for x in out if len(x) >= 6), key=len, reverse=True)


def redact(text, secrets):
    for s in secrets:
        text = text.replace(s, "<redacted>")
    return text


def redact_obj(obj, secrets):
    if isinstance(obj, str):
        return redact(obj, secrets)
    if isinstance(obj, list):
        return [redact_obj(x, secrets) for x in obj]
    if isinstance(obj, dict):
        return {k: redact_obj(v, secrets) for k, v in obj.items()}
    return obj


def find_secrets(text, secrets):
    return [s for s in secrets if s in text]


# ---- the checks ------------------------------------------------------------------------------------------------------

def accepted_findings(rules):
    """ocvalidate findings that the driver's own settings cause on purpose, each with the reason and the exact value it
    is accepted for. Anything else ocvalidate reports fails the build."""
    bars = next(i["value"] for i in rules["settings"] if i["path"] == "Booter.Quirks.ResizeAppleGpuBars")
    return [(r"^Booter->Quirks->ResizeAppleGpuBars is set to {}, which is unstable with macOS sleep-wake!$".format(bars),
             "Booter",
             "stage 2 sets ResizeAppleGpuBars = {} (a 1 GB window on the card's VRAM, efi/README.md); ocvalidate accepts "
             "only 0 and -1 and calls 9 and 10 unstable with sleep-wake because macOS does not restore the BAR size after "
             "sleep; IntelXe.kext does, and sleep and wake work with it on the test machine "
             "(docs/design/DESIGN-rebar-window.md)".format(bars))]


def run_ocvalidate(tools, config_path, secrets, rules):
    exe = tools.path("ocvalidate")
    try:
        # run in the config's folder with a relative name: ocvalidate opens its argument through the ANSI file API on
        # Windows, which cannot open a path with characters outside the code page (a user folder named in Turkish)
        p = subprocess.run([exe, os.path.basename(config_path)], cwd=os.path.dirname(os.path.abspath(config_path)),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise ValidatorUnavailable("ocvalidate did not run: {}".format(e))
    text = redact(p.stdout.decode("utf-8", "replace"), secrets)
    done = re.search(r"Completed validating .*?(?:Found (\d+) issues? requiring attention|No issues found)", text)
    if not done:
        raise ValidatorUnavailable("ocvalidate did not complete (exit {}): {}".format(p.returncode, text[-500:]))
    found = int(done.group(1) or 0)
    lines = [l.strip() for l in text.splitlines() if l.strip() and not l.startswith(("NOTE:", "Completed validating"))]
    accepted, issues, per_section = [], [], {}
    rules_accepted = accepted_findings(rules)
    for line in lines:
        m = re.match(r"^Check(\w+) returns (\d+) errors?!$", line)
        if m:
            per_section[m.group(1)] = int(m.group(2))
            continue
        hit = next(((sec, why) for rx, sec, why in rules_accepted if re.match(rx, line)), None)
        if hit:
            accepted.append({"finding": line, "section": hit[0], "why": hit[1]})
        else:
            issues.append(line)
    # every error ocvalidate counted must be one of the lines it printed and accepted here
    counted_ok = found == len(accepted) and all(
        per_section.get(sec, 0) == sum(1 for a in accepted if a["section"] == sec) for sec in per_section)
    ok = (p.returncode == 0 and found == 0) or (not issues and accepted and counted_ok)
    if not ok and not issues:
        issues = ["ocvalidate counted {} issue(s) that it did not print as known lines".format(found)]
    return {"ok": ok, "exit": p.returncode, "found": found, "accepted": accepted, "issues": issues, "text": text.strip()}


def _entries(config, section, key):
    node = config
    for k in section.split("."):
        node = (node or {}).get(k)
    return [e for e in (node or []) if isinstance(e, dict)] if isinstance(node, list) else []


def validate(out_dir, cache_root=None, report=None, policy=None, secrets=None, after_stage4=False):
    """after_stage4: the EFI after efi.sh/IntelXe-EFI.ps1/the app made stage 4 in it (the tests do that on a copy):
    IntelXe.kext and SSDT-ARCOFF.aml must then be there and enabled, HybridPM.kext below Lilu.kext when the CPU needs it."""
    cache = pins.Cache(cache_root)
    lock = pins.Lock()
    tools = Tools(lock, cache)
    rules = rules_mod.load()
    efi = os.path.join(out_dir, "EFI")
    oc = os.path.join(efi, "OC")
    config_path = os.path.join(oc, "config.plist")
    secrets = secrets if secrets is not None else read_secrets(config_path)
    checks, failures, notes = {}, [], []
    with open(config_path, "rb") as f:
        config = plistlib.load(f)

    # 1. ocvalidate of the same release
    try:
        checks["ocvalidate"] = run_ocvalidate(tools, config_path, secrets, rules)
        if not checks["ocvalidate"]["ok"]:
            failures.append("ocvalidate: " + "; ".join(checks["ocvalidate"]["issues"][:8]))
        for a in checks["ocvalidate"]["accepted"]:
            notes.append("ocvalidate: '{}' accepted: {}".format(a["finding"], a["why"]))
    except ValidatorUnavailable as e:
        checks["ocvalidate"] = {"ok": False, "error": str(e)}
        failures.append("ocvalidate could not run: {}".format(e))

    # 2. the OpenCore files are the pinned release's
    same = []
    for member, rel in (("X64/EFI/OC/OpenCore.efi", "OC/OpenCore.efi"), ("X64/EFI/BOOT/BOOTx64.efi", "BOOT/BOOTx64.efi")):
        want = tools.release_file(member)
        path = os.path.join(efi, *rel.split("/"))
        got = open(path, "rb").read() if os.path.isfile(path) else None
        if want is None or got != want:
            failures.append("{} is not the pinned OpenCore {}'s".format(rel, lock.products["OpenCorePkg"].get("version", "")))
        else:
            same.append(rel)
    for d in _entries(config, "UEFI.Drivers", "Path"):
        if not d.get("Enabled"):
            continue
        rel = "OC/Drivers/" + d["Path"]
        path = os.path.join(efi, *rel.split("/"))
        if not os.path.isfile(path):
            failures.append("UEFI > Drivers names {}, which is not in the EFI".format(d["Path"]))
            continue
        want = tools.release_file("X64/EFI/" + rel)
        if want is not None:
            if open(path, "rb").read() != want:
                failures.append("{} is not the pinned OpenCore's".format(rel))
            else:
                same.append(rel)
    checks["opencore_release"] = {"version": lock.products["OpenCorePkg"].get("version", ""), "same_bytes": same}

    # 3. stage 2 complete
    which = "settings-safari" if (policy is None or policy.safari) else "settings"
    left, lines, n = rules_mod.plan(rules, config, which)
    checks["stage2"] = {"plan": which, "left": lines, "notes": n}
    if left:
        failures.append("stage 2 is not complete: " + "; ".join(lines))

    # 4. stage 4 not yet, and nothing in its way
    kexts = _entries(config, "Kernel.Add", "BundlePath")
    acpi = _entries(config, "ACPI.Add", "Path")
    names = [k.get("BundlePath", "") for k in kexts]
    bad = []
    if after_stage4:
        if not any(k.get("BundlePath") == rules["kext"]["bundle"] and k.get("Enabled") for k in kexts):
            bad.append("stage 4 did not add IntelXe.kext")
        if not any(a.get("Path") == rules["ssdt"]["file"] and a.get("Enabled") for a in acpi):
            bad.append("stage 4 did not add SSDT-ARCOFF.aml")
        cpu = ((report or {}).get("CPU") or {}).get("Processor Name", "")
        if cpu and rules_mod.needs_hybridpm(rules, cpu):
            order = [k.get("BundlePath") for k in kexts]
            h, l = rules["hybridpm"]["bundle"], rules["hybridpm"]["after"]
            if h not in order or l not in order or order.index(h) != order.index(l) + 1:
                bad.append("stage 4 did not put HybridPM.kext right below Lilu.kext")
    else:
        if rules["kext"]["bundle"] in names or os.path.exists(os.path.join(oc, "Kexts", rules["kext"]["bundle"])):
            bad.append("IntelXe.kext is in the EFI (it belongs to stage 4, after the driver is installed)")
        if any(a.get("Path") == rules["ssdt"]["file"] for a in acpi) or os.path.exists(os.path.join(oc, "ACPI", rules["ssdt"]["file"])):
            bad.append("SSDT-ARCOFF.aml is in the EFI: it hides the card from the display driver the installer needs")
    if any(n.lower() == "whatevergreen.kext" for n in names):
        bad.append("WhateverGreen.kext is in the EFI (the guide: no WhateverGreen for the card)")
    for a in acpi:
        if a.get("Enabled") and str(a.get("Path", "")).startswith("SSDT-Disable_GPU"):
            bad.append("{} turns a discrete GPU off".format(a["Path"]))
    for b in _entries(config, "Kernel.Block", "Identifier"):
        if b.get("Enabled") and b.get("Identifier") == "com.apple.iokit.IONDRVSupport":
            bad.append("Kernel > Block excludes IONDRVSupport: the installer would have no display")
    card_paths = []
    if report:
        for p in (report.get("GPU") or {}).values():
            if rules_mod.is_card(rules, p.get("Device ID")) and p.get("PCI Path"):
                card_paths.append(p["PCI Path"])
    props = (config.get("DeviceProperties") or {}).get("Add") or {}
    for path in card_paths:
        if path in props:
            bad.append("DeviceProperties for the card ({}): {}".format(path, ", ".join(props[path])))
    checks["card"] = {"pci_paths": card_paths, "problems": bad}
    failures += bad

    # 5. every entry's file
    missing = []
    for k in kexts:
        if not k.get("Enabled"):
            continue
        base = os.path.join(oc, "Kexts", *k.get("BundlePath", "").split("/"))
        for sub in (k.get("ExecutablePath"), k.get("PlistPath")):
            if sub and not os.path.isfile(os.path.join(base, *sub.split("/"))):
                missing.append("{}/{}".format(k.get("BundlePath"), sub))
    for a in acpi:
        if a.get("Enabled") and not os.path.isfile(os.path.join(oc, "ACPI", a.get("Path", ""))):
            missing.append("ACPI/" + a.get("Path", ""))
    checks["files"] = {"missing": missing}
    failures += ["missing " + m for m in missing]

    # 6. what stage 4 will find
    lilu = next((k for k in kexts if k.get("BundlePath") == rules["hybridpm"]["after"]), None)
    esd = ((config.get("Misc") or {}).get("Security") or {}).get("ExposeSensitiveData")
    cpu = ((report or {}).get("CPU") or {}).get("Processor Name", "")
    s4 = {"lilu": bool(lilu and lilu.get("Enabled")), "hybridpm_needed": rules_mod.needs_hybridpm(rules, cpu) if cpu else None,
          "boot_path_recorded_by_opencore": isinstance(esd, int) and bool(esd & int(rules["boot_path"]["bits"], 0))}
    if s4["hybridpm_needed"] and not s4["lilu"]:
        notes.append("HybridPM.kext needs Lilu.kext, which this EFI does not load: stage 4 will leave it out")
    checks["stage4"] = s4
    return {"ok": not failures, "failures": failures, "notes": notes, "checks": checks}
