# SPDX-License-Identifier: MIT
"""The install guide's own rules for the EFI OpCore Simplify built, before stage 2 (rules.py) is applied.

Stage 1 of release/README-INSTALL.md (site/guide/macos.html) installs macOS with a plain OpenCore EFI on the card's
firmware output, as on the test machine (efi/sample-z790/README.md, "For the macOS installer"):

- nothing GPU-specific for the card (no IntelXe.kext, SSDT-ARCOFF.aml, DeviceProperties or WhateverGreen): the engine
  keeps the card away from OpCore Simplify's GPU handling, validate.py checks the result;
- ResizeAppleGpuBars = 10 and ReconnectGraphicsOnConnect = false: stage 2 of the rules sets both;
- CpuTopologyRebuild.kext off and its ctrsmt= boot-arg removed for the install, as the test machine installed macOS
  (with -v it warns of random boot failures, and the test machine saw them); after the setup assistant it goes back on
  with ctrsmt=off (the notes say so);
- OpenCore records the EFI it booted from (Misc > Security > ExposeSensitiveData bit 0, the rules' boot_path), so that
  efi.sh and IntelXe Manager find this EFI for stages 2 to 4 on macOS.

Each change is returned with its reason for the build log.
"""
import plistlib
import re


NVRAM = "7C436110-AB2A-4BBB-A880-FE41995C9F82"


def apply(config_path, report, facts, policy, rules):
    with open(config_path, "rb") as f:
        config = plistlib.load(f)
    changes = []

    def change(what, why):
        changes.append({"step": "stage 1 (install guide)", "change": what, "why": why})

    # CpuTopologyRebuild: off for the install, as on the test machine
    for k in (config.get("Kernel") or {}).get("Add") or []:
        if k.get("BundlePath") == "CpuTopologyRebuild.kext" and k.get("Enabled"):
            k["Enabled"] = False
            change("Kernel > Add: CpuTopologyRebuild.kext off",
                   "the test machine installed macOS without it; it warns of random boot failures with -v (efi/sample-z790/README.md)")
    if not any(k.get("BundlePath") == "CpuTopologyRebuild.kext" and k.get("Enabled") for k in (config.get("Kernel") or {}).get("Add") or []):
        for p in (config.get("Kernel") or {}).get("Patch") or []:
            if str(p.get("Comment", "")).startswith("force HT enabled") and p.get("Enabled"):
                p["Enabled"] = False
                change("Kernel > Patch: '{}' off".format(p["Comment"]),
                       "OpCore Simplify adds it with CpuTopologyRebuild (its patches_ht.plist); the test machine has neither")
    args = ((config.get("NVRAM") or {}).get("Add") or {}).get(NVRAM, {}).get("boot-args")
    if isinstance(args, str) and re.search(r"(^| )ctrsmt=\S*", args):
        new = " ".join(a for a in args.split() if not a.startswith("ctrsmt="))
        config["NVRAM"]["Add"][NVRAM]["boot-args"] = new
        change("boot-args: ctrsmt= removed ('{}' -> '{}')".format(args, new), "it belongs to CpuTopologyRebuild, which is off for the install")

    # OpenCore records which EFI it booted from
    sec = (config.get("Misc") or {}).get("Security")
    bits = int(rules["boot_path"]["bits"], 0)
    if isinstance(sec, dict):
        cur = sec.get("ExposeSensitiveData")
        if isinstance(cur, int) and not isinstance(cur, bool) and cur & bits != bits:
            sec["ExposeSensitiveData"] = cur | bits
            change("Misc > Security > ExposeSensitiveData: {} -> {}".format(cur, cur | bits),
                   "OpenCore then records the EFI it booted from; efi.sh and IntelXe Manager use it in stages 2 to 4")

    with open(config_path, "wb") as f:
        plistlib.dump(config, f, sort_keys=False)
    return changes


def firmware_steps(report, facts):
    """The firmware settings the guide asks for (stage 1, step 1), and the ones this report adds."""
    steps = ["Above 4G Decoding: on", "Resizable BAR: on (the build sets ResizeAppleGpuBars = 10 for it)",
             "CSM: off (UEFI boot only)", "Secure Boot: off"]
    if facts.get("igpus"):
        steps.append("Integrated graphics ({}): off, or at least not the primary display; the monitor goes on the "
                     "Arc card's DisplayPort".format(", ".join(facts["igpus"])))
    else:
        steps.append("Primary display: the PCIe card; the monitor goes on the Arc card's DisplayPort")
    if (report.get("BIOS") or {}).get("Firmware Type") not in (None, "UEFI"):
        steps.append("Boot mode: UEFI (the report says {})".format(report["BIOS"]["Firmware Type"]))
    return steps


def after_install_notes(config_path):
    notes = ["After the setup assistant: stage 3 (install the driver on macOS), then stage 4 (IntelXe.kext, "
             "SSDT-ARCOFF.aml and, on a 12th-14th gen hybrid CPU, HybridPM.kext) with IntelXe Manager or efi.sh, "
             "before the next restart (release/README-INSTALL.md)."]
    with open(config_path, "rb") as f:
        config = plistlib.load(f)
    kexts = [k.get("BundlePath") for k in (config.get("Kernel") or {}).get("Add") or []]
    if "CpuTopologyRebuild.kext" in kexts:
        notes.append("Then CpuTopologyRebuild.kext back on with the boot-arg ctrsmt=off, and -v removed, as on the "
                     "test machine (efi/sample-z790/README.md).")
    if "UTBDefault.kext" in kexts:
        notes.append("USB: UTBDefault.kext is a stand-in; map the ports (USBToolBox on Windows) and replace it with "
                     "your UTBMap.kext.")
    return notes
