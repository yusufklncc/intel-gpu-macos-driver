# SPDX-License-Identifier: MIT
"""Command line of the EFI builder.

  efibuild.py build --report Report.json --acpi ACPI_DIR --out OUT_DIR [--macos 26|15] [--iasl PATH]
                    [--smbios MODEL] [--no-safari] [--log build-log.txt] [--result result.json]
      Build an OpenCore EFI into OUT_DIR/EFI (OUT_DIR must be empty or an earlier build's), apply stage 2 and check it
      with ocvalidate. Exit 0 only when every step passed; the result JSON says what was decided and why.
  efibuild.py validate --efi OUT_DIR [--report Report.json] [--after-stage4]
      ocvalidate (from the pinned OpenCore release) and the stage-2/stage-4 checks on an existing build.
  efibuild.py upstream --fetch | --manifest NAME
      Fetch and check the pinned OpCore Simplify and Hardware Sniffer, or (maintainer) write a manifest.
  efibuild.py lock --report ... --acpi ... --out ... (maintainer)
      Run a build that resolves what OpCore Simplify would download today and records it in downloads.lock.json.
  efibuild.py vectors
      Run the stage-2 and Safari vectors of release/efi-rules-vectors.json through rules.py.
  efibuild.py sniff --out DIR  (Windows, administrator)
      Run the pinned Hardware Sniffer with the pinned acpidump.exe: DIR/Report.json and DIR/ACPI/ for a build.
"""
import argparse
import json
import sys

from . import pins
from . import upstream


def _cmd_upstream(args):
    if args.manifest:
        digest, count = upstream.write_manifest(args.manifest, args.cache)
        print("{}: {} files, manifest_sha256 {}".format(args.manifest, count, digest))
        return 0
    for name in upstream.pinned():
        print("{}: {}".format(name, upstream.verify(name, upstream.fetch(name, args.cache))))
    return 0


def _cmd_build(args, lock_mode=False):
    from . import engine  # noqa: PLC0415 - imports the rest only for a build
    policy = engine.Policy(macos=args.macos, smbios=args.smbios, safari=not args.no_safari)
    result = engine.build(args.report, args.acpi, args.out, policy=policy, iasl=args.iasl, cache_root=args.cache,
                          log_path=args.log, lock_mode=lock_mode, echo=sys.stderr if args.verbose else None)
    if args.result:
        with open(args.result, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    print("")
    print("result: {}".format("ok" if result["ok"] else "FAILED: " + result.get("error", "")))
    return 0 if result["ok"] else 1


def _cmd_validate(args):
    from . import validate  # noqa: PLC0415
    report_json = None
    if args.report:
        with open(args.report, encoding="utf-8") as f:
            report_json = json.load(f)
    report = validate.validate(args.efi, cache_root=args.cache, report=report_json, after_stage4=args.after_stage4)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


def _cmd_sniff(args):
    from . import sniff  # noqa: PLC0415
    try:
        report, acpi, kept = sniff.sniff(args.out, args.cache)
    except sniff.SniffError as e:
        print("error: {}".format(e), file=sys.stderr)
        return 2
    print("report: {}\nacpi: {} ({})".format(report, acpi, ", ".join(kept)))
    return 0


def _cmd_vectors(args):
    from . import rules  # noqa: PLC0415
    failures, count = rules.run_vectors()
    for f in failures:
        print("FAIL " + f)
    print("{} of {} stage-2/Safari vectors pass".format(count - len(failures), count))
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="efibuild.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cache", help="download cache (default: %%LOCALAPPDATA%%\\IntelXe\\efibuild or ~/.cache/intelxe-efibuild)")
    sub = p.add_subparsers(dest="cmd", required=True)

    for name in ("build", "lock"):
        b = sub.add_parser(name)
        b.add_argument("--report", required=True, help="Hardware Sniffer Report.json")
        b.add_argument("--acpi", required=True, help="folder of ACPI tables (acpidump -b output: dsdt.dat, ssdt*.dat or .aml)")
        b.add_argument("--out", required=True, help="output folder; EFI/ is written inside it")
        b.add_argument("--macos", default="26", choices=("26", "15"), help="macOS 26 Tahoe (default) or 15 Sequoia")
        b.add_argument("--smbios", default=None, help="Mac model (default: the rule in policy.py)")
        b.add_argument("--no-safari", action="store_true", help="leave Misc > Security > SecureBootModel as built")
        b.add_argument("--iasl", default=None, help="iasl to disassemble the tables (Windows default: the pinned ACPICA iasl.exe)")
        b.add_argument("--log", default=None, help="build log (default: OUT/build-log.txt)")
        b.add_argument("--result", default=None, help="write the result as JSON here")
        b.add_argument("--verbose", action="store_true", help="show OpCore Simplify's output and the answers as they come")

    v = sub.add_parser("validate")
    v.add_argument("--efi", required=True, help="a build's output folder (holding EFI/OC/config.plist)")
    v.add_argument("--report", help="the Report.json it was built from (enables the card and CPU checks)")
    v.add_argument("--after-stage4", action="store_true", help="the EFI after stage 4 was made in it (tests)")

    u = sub.add_parser("upstream")
    g = u.add_mutually_exclusive_group(required=True)
    g.add_argument("--fetch", action="store_true")
    g.add_argument("--manifest", choices=("OpCore-Simplify", "Hardware-Sniffer"))

    sub.add_parser("vectors")

    sn = sub.add_parser("sniff")
    sn.add_argument("--out", required=True, help="folder for Report.json and ACPI/ (Windows, as administrator)")

    args = p.parse_args(argv)
    try:
        if args.cmd == "upstream":
            return _cmd_upstream(args)
        if args.cmd == "build":
            return _cmd_build(args)
        if args.cmd == "lock":
            return _cmd_build(args, lock_mode=True)
        if args.cmd == "validate":
            return _cmd_validate(args)
        if args.cmd == "vectors":
            return _cmd_vectors(args)
        if args.cmd == "sniff":
            return _cmd_sniff(args)
    except pins.PinError as e:
        print("error: {}".format(e), file=sys.stderr)
        return 2
    return 2
