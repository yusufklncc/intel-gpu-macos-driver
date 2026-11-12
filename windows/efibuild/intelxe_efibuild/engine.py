# SPDX-License-Identifier: MIT
"""Runs OpCore Simplify without a console and builds an EFI for the driver.

The program is used as it is, at the pinned commit (upstream.py), through its own main menu. What is replaced:

- Utils.request_input: every question goes to answers.Answerer (a rule per question, recorded; none means stop);
- Utils.head / progress_bar / adjust_window_size / open_folder / exit_program: no console control; head records the
  screen title the answers are looked up by; exit_program stops the build with what the program printed;
- downloads (ResourceFetcher, gatheringFiles.fetch_latest_products_info): only what downloads.lock.json pins
  (pinned.py), checked by SHA-256; the program's cache goes to our cache folder;
- iasl and macserial: the iasl given to the builder, macserial from the pinned OpenCore release (validate.tool);
- the Arc card: OpCore Simplify knows no macOS driver for it and would stop ("You cannot install macOS without a
  supported GPU") or turn it off. For the compatibility check the card counts as supported on every macOS (the
  installer and macOS before stage 4 run on its firmware output), and the kext selection does not see it, so no
  WhateverGreen, DeviceProperties or "disable-gpu" is made for it.

After the program: policy.py (the install guide's stage-1 rules), stage 2 from efi-rules.json (rules.py), and the
checks (validate.py). The result is a dict (and JSON with --result) with every answer and change and the reasons.
"""
import contextlib
import datetime
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tempfile
import types
import zipfile

from . import answers
from . import pins
from . import policy as policy_mod
from . import rules as rules_mod
from . import upstream
from . import validate

MARKER = ".intelxe-efibuild"     # an output folder holding this file was made by the builder and may be replaced
MACOS = {"26": ("25", "macOS 26 Tahoe"), "15": ("24", "macOS 15 Sequoia")}


class Policy:
    """The choices a user of the Windows app does not have to make. Each default says why."""

    def __init__(self, macos="26", smbios=None, safari=True):
        if macos not in MACOS:
            raise ValueError("macOS must be 26 or 15")
        self.macos = macos
        self.darwin, self.macos_name = MACOS[macos]
        # MacBookPro16,2: the model the driver is tested with on both macOS 15 and 26 (efi/sample-z790). OpCore
        # Simplify would pick MacPro7,1 (26) or iMacPro1,1 (15) for a desktop with a discrete GPU; neither has been
        # run with the B580.
        self.smbios = smbios or "MacBookPro16,2"
        self.smbios_why = "the Mac model the driver is tested with (efi/sample-z790)" if not smbios else "chosen with --smbios"
        # SecureBootModel Disabled: the test machine needs it on macOS 26 (it looped at macos_seal_system_volume with
        # Default) and the system-volume copy for Safari needs it; OpCore Simplify already sets it for macOS 14+.
        self.safari = safari

    def as_dict(self):
        return {"macos": self.macos, "darwin": self.darwin, "smbios": self.smbios, "safari": self.safari}


class Tee(io.TextIOBase):
    """stdout during the run: kept for the log and for the answers' context, not shown unless echo."""

    def __init__(self, echo=None):
        self.parts, self.echo = [], echo

    def write(self, s):
        self.parts.append(s)
        if self.echo:
            self.echo.write(s)
        return len(s)

    def text(self):
        return answers.clean("".join(self.parts))


class Answerer:
    class Done(Exception):
        """The main menu came back after the build: the program's part is over."""

    def __init__(self, policy, inputs, facts, tee):
        self.policy, self.inputs, self.facts, self.tee = policy, inputs, facts, tee
        self.state, self.decisions, self.notices = {}, [], []
        self.screen, self.prompt, self.context = "", "", ""
        self._mark, self._last, self._repeat = 0, None, 0

    def head(self, text=None):
        self.screen = (text or "OpCore Simplify").strip()
        print("\n== {}".format(self.screen))

    def _take_context(self):
        text = self.tee.text()
        ctx, self._mark = text[self._mark:], len(text)
        return "\n".join(line.rstrip() for line in ctx.splitlines() if line.strip())

    def ask(self, prompt):
        self.prompt = answers.clean(prompt).strip()
        self.context = self._take_context()
        key = (self.screen, self.prompt)
        self._repeat = self._repeat + 1 if key == self._last else 1
        self._last = key
        for rid, screen_re, prompt_re, rule in answers.RULES:
            if answers.re.search(screen_re, self.screen) and answers.re.search(prompt_re, self.prompt):
                if self._repeat > answers.REPEATS.get(rid, 20 if self.prompt.startswith("Press Enter") else 1):
                    raise answers.Stop("OpCore Simplify asked {!r} again on {!r}: it did not accept the answer.\n{}".format(
                        self.prompt, self.screen, self.context[-1500:]))
                answer, why = rule(self)
                if why == "notice":
                    self.notices.append("[{}] {}".format(self.screen, self.context))
                else:
                    self.decisions.append({"rule": rid, "screen": self.screen, "question": self.prompt,
                                           "answer": answer, "why": why})
                print("{}{}".format(self.prompt, answer))
                print("   [{}: {}]".format(rid, why))
                self._mark = len(self.tee.text())
                return answer
        raise answers.Stop("no rule answers {!r} on the screen {!r}. OpCore Simplify had printed:\n{}".format(
            self.prompt, self.screen, self.context[-2000:]))


# ---- patches into the upstream modules -------------------------------------------------------------------------------

class Patches:
    """Everything the builder changes in OpCore Simplify's modules, applied after import, before OCPE() is made."""

    def __init__(self, upstream_dir, answerer, lock, cache, ock_dir, iasl, tools, rules, lock_mode, log):
        self.dir, self.a, self.lock, self.cache, self.ock_dir = upstream_dir, answerer, lock, cache, ock_dir
        self.iasl, self.tools, self.rules, self.lock_mode, self.log = iasl, tools, rules, lock_mode, log
        self.used_products, self.downloads = {}, []

    def install(self):
        from Scripts import utils, dsdt, smbios, gathering_files, kext_maestro, resource_fetcher, compatibility_checker  # noqa
        from Scripts.datasets import os_data  # noqa
        a, me = self.a, self

        U = utils.Utils
        U.request_input = lambda self_, prompt="Press Enter to continue...": a.ask(prompt)
        U.head = lambda self_, text=None, width=68, resize=True: a.head(text)
        U.adjust_window_size = lambda self_, content="": None
        U.open_folder = lambda self_, folder_path: None
        U.clean_temporary_dir = lambda self_: None

        def exit_program(self_):
            raise answers.Stop("OpCore Simplify quit. It printed:\n" + "\n".join(a.notices[-2:] + [a._take_context()])[-2500:])
        U.exit_program = exit_program

        def progress_bar(self_, title, steps, current_step_index, done=False):
            a.screen = title
            print("[{}] {}".format(title, "done" if done else steps[current_step_index] if current_step_index < len(steps) else ""))
        U.progress_bar = progress_bar

        # no pauses for effect
        for mod in (compatibility_checker,):
            mod.time = types.SimpleNamespace(sleep=lambda s: None, time=__import__("time").time)

        # iasl: the one given (Windows: iasl.exe from the pinned ACPICA release), never a download
        dsdt.DSDT.check_iasl = lambda self_, legacy=False, try_downloading=True: None if legacy else me.iasl

        # macserial: from the pinned OpenCore release, checked
        smbios.SMBIOS.check_macserial = lambda self_, retry_count=0: me.tools("macserial")

        # the program's download cache lives in ours, not beside its code
        def gf_init(orig):
            def init(self_, *args, **kw):
                orig(self_, *args, **kw)
                self_.ock_files_dir = me.ock_dir
                self_.download_history_file = os.path.join(me.ock_dir, "history.json")
            return init
        gathering_files.gatheringFiles.__init__ = gf_init(gathering_files.gatheringFiles.__init__)

        def km_init(orig):
            def init(self_, *args, **kw):
                orig(self_, *args, **kw)
                self_.ock_files_dir = me.ock_dir
            return init
        kext_maestro.KextMaestro.__init__ = km_init(kext_maestro.KextMaestro.__init__)

        # the products: from the lock; in lock mode from the program's own resolution, recorded
        original_products = gathering_files.gatheringFiles.fetch_latest_products_info

        class Recording(dict):
            def get(self_, key, default=None):
                value = dict.get(self_, key, default)
                if value is not None:
                    me.used_products[key] = value
                return value

        def fetch_latest_products_info(self_, kexts):
            if me.lock_mode:
                from . import lock as lock_mod  # noqa: PLC0415
                products = lock_mod.resolve_products(original_products(self_, kexts))
            else:
                products = {k: {"product_name": k, "id": v.get("id", k), "url": v["url"], "sha256": v["sha256"]}
                            for k, v in me.lock.products.items()}
            return Recording(products)
        gathering_files.gatheringFiles.fetch_latest_products_info = fetch_latest_products_info

        RF = resource_fetcher.ResourceFetcher
        original_fetch = RF.fetch_and_parse_content

        def download_and_save_file(self_, resource_url, destination_path, sha256_hash=None):
            pin = me.lock.pin_for_url(resource_url)
            if pin is None and me.lock_mode:
                from . import lock as lock_mod  # noqa: PLC0415
                pin = lock_mod.pin_file(me.lock, resource_url) or lock_mod.pin_download(
                    me.lock, me.used_products, resource_url, me.cache)
            if pin is None:
                raise pins.PinError("OpCore Simplify wants {}, which downloads.lock.json does not pin".format(resource_url))
            url, digest, entry = pin
            if sha256_hash and sha256_hash.lower() != digest.lower():
                raise pins.PinError("{}: the program expects SHA-256 {}, the lock pins {}".format(resource_url, sha256_hash, digest))
            path = me.cache.fetch(url, digest)
            if entry.get("top_folder"):
                _rename_zip_top(path, destination_path, entry["top_folder"])
            else:
                pins.copy_verified(path, destination_path, digest)
            me.downloads.append({"url": url, "sha256": digest, "for": resource_url})
            print("pinned download: {} (SHA-256 {})".format(url, digest))
            return True
        RF.download_and_save_file = download_and_save_file

        def fetch_and_parse_content(self_, resource_url, content_type=None):
            pin = me.lock.pin_for_url(resource_url)
            if pin is None and me.lock_mode:
                from . import lock as lock_mod  # noqa: PLC0415
                pin = lock_mod.pin_file(me.lock, resource_url)
                if pin is None:
                    return original_fetch(self_, resource_url, content_type)
            if pin is None:
                raise pins.PinError("OpCore Simplify wants {}, which downloads.lock.json does not pin".format(resource_url))
            url, digest, _ = pin
            data = me.cache.read(url, digest)
            me.downloads.append({"url": url, "sha256": digest, "for": resource_url})
            if content_type == "json":
                return json.loads(data)
            if content_type == "plist":
                import plistlib  # noqa: PLC0415
                return plistlib.loads(data)
            return data.decode("utf-8")
        RF.fetch_and_parse_content = fetch_and_parse_content

        # the Arc card in the compatibility check: supported on every macOS (firmware output); the other GPUs as the
        # program decides
        CC = compatibility_checker.CompatibilityChecker
        original_gpu_check = CC.check_gpu_compatibility
        rules = self.rules

        def check_gpu_compatibility(self_):
            gpus = self_.hardware_report.get("GPU") or {}
            cards = {n: p for n, p in gpus.items() if rules_mod.is_card(rules, p.get("Device ID"))}
            if not cards:
                return original_gpu_check(self_)
            full = (os_data.get_latest_darwin_version(), os_data.get_lowest_darwin_version())
            rest = {n: p for n, p in gpus.items() if n not in cards}
            widest = self_._widest_compatibility
            try:
                self_._widest_compatibility = lambda comps: widest(list(comps) + [full])
                self_.hardware_report["GPU"] = rest
                if rest:
                    original_gpu_check(self_)
            finally:
                del self_._widest_compatibility
                self_.hardware_report["GPU"] = gpus
            for name, props in cards.items():
                props["Compatibility"] = full
                print("   - {}: firmware output (no macOS driver until stage 4 of the install guide)".format(name))
        CC.check_gpu_compatibility = check_gpu_compatibility

        # the kext selection without the Arc card: nothing for it (no WhateverGreen)
        KM = kext_maestro.KextMaestro
        original_select = KM.select_required_kexts

        def select_required_kexts(self_, hardware_report, macos_version, needs_oclp, acpi_patches):
            gpus = hardware_report.get("GPU") or {}
            view = dict(hardware_report)
            view["GPU"] = {n: p for n, p in gpus.items() if not rules_mod.is_card(rules, p.get("Device ID"))}
            return original_select(self_, view, macos_version, needs_oclp, acpi_patches)
        KM.select_required_kexts = select_required_kexts

        # the DSDT is found by its signature: the program reads it as bytes and compares with a str, so its DSDT
        # pre-patch path never runs; a bytes value that also equals the str restores it
        original_sig = dsdt.DSDT._table_signature

        class Sig(bytes):
            def __eq__(self_, other):
                return bytes.__eq__(self_, other.encode("latin-1") if isinstance(other, str) else other)

            def __ne__(self_, other):
                return not self_.__eq__(other)
            __hash__ = bytes.__hash__

        def table_signature(self_, table_path, table_name=None):
            s = original_sig(self_, table_path, table_name=table_name)
            return Sig(s) if isinstance(s, bytes) else s
        dsdt.DSDT._table_signature = table_signature


def _rename_zip_top(src, dst, top):
    """A copy of a GitHub archive zip with its top folder renamed (OcBinaryData-<commit> -> OcBinaryData-master, the
    name the program looks for)."""
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for info in zin.infolist():
            parts = info.filename.split("/", 1)
            name = top + ("/" + parts[1] if len(parts) > 1 else "/")
            data = b"" if info.is_dir() else zin.read(info.filename)
            new = zipfile.ZipInfo(name, date_time=info.date_time)
            new.external_attr = info.external_attr
            zout.writestr(new, data)


# ---- the build -------------------------------------------------------------------------------------------------------

def _prepare_out(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    entries = [e for e in os.listdir(out_dir) if not e.startswith(".DS_Store")]
    if entries and MARKER not in entries:
        raise pins.PinError("{} is not empty and was not made by the builder: choose an empty folder".format(out_dir))
    for e in entries:
        p = os.path.join(out_dir, e)
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p)
        else:
            os.remove(p)
    with open(os.path.join(out_dir, MARKER), "w") as f:
        f.write("made by windows/efibuild (intel-gpu-macos-driver); the builder may replace this folder\n")


def _home_forms():
    home = os.path.expanduser("~")
    if len(home) <= 3:
        return []
    return sorted({home, home.replace("\\", "/"), os.path.realpath(home)}, key=len, reverse=True)


def _load_report(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _precheck(report, rules, policy):
    """Facts the program does not judge, decided before it runs."""
    facts, stops = {}, []
    gpus = report.get("GPU") or {}
    cards = [n for n, p in gpus.items() if rules_mod.is_card(rules, p.get("Device ID"))]
    facts["cards"] = cards
    facts["igpus"] = [n for n, p in gpus.items() if p.get("Device Type") == "Integrated GPU"]
    facts["other_gpus"] = [n for n in gpus if n not in cards and n not in facts["igpus"]]
    facts["cpu"] = (report.get("CPU") or {}).get("Processor Name", "")
    if not cards:
        stops.append("no Arc B580 or B570 in the report (PCI 8086:{}): this builder makes an EFI for the driver's cards".format(
            "/".join(rules["cards"]["device_ids"])))
    if (report.get("CPU") or {}).get("Manufacturer") != "Intel":
        stops.append("the driver is tested on Intel CPUs only; this report's CPU is {}".format(facts["cpu"] or "unknown"))
    facts["hybridpm"] = rules_mod.needs_hybridpm(rules, facts["cpu"])
    return facts, stops


def build(report_path, acpi_dir, out_dir, policy=None, iasl=None, cache_root=None, log_path=None, lock_mode=False,
          echo=None):
    policy = policy or Policy()
    started = datetime.datetime.now()
    result = {"ok": False, "builder": "windows/efibuild", "policy": policy.as_dict(), "decisions": [], "notices": [],
              "changes": [], "downloads": [], "checks": {}, "started": started.isoformat(timespec="seconds")}
    out_dir = os.path.abspath(out_dir)
    log_path = log_path or os.path.join(out_dir, "build-log.txt")
    cache = pins.Cache(cache_root)
    lock = pins.Lock()
    rules = rules_mod.load()
    pinned = upstream.pinned()
    result["upstream"] = {k: v["commit"] for k, v in pinned.items()}
    result["opencore"] = lock.products.get("OpenCorePkg", {}).get("version", "")
    secrets = []
    log = io.StringIO()

    def finish(error=None):
        result["finished"] = datetime.datetime.now().isoformat(timespec="seconds")
        if error:
            result["ok"] = False
            result["error"] = error
        homes = _home_forms()   # the user's folder name stays out of a log that may be shared
        blob = json.dumps(result)
        for h in homes:
            blob = blob.replace(json.dumps(h)[1:-1], "~")
        result.update(json.loads(blob))
        text = validate.redact(log.getvalue() + "\n" + json.dumps(result, indent=2), secrets)
        for h in homes:
            text = text.replace(h, "~")
        os.makedirs(os.path.dirname(log_path), exist_ok=True)
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(text)
        leaked = validate.find_secrets(text, secrets)
        if leaked:   # cannot happen after redact; kept as the last line of defence
            result["ok"] = False
            result["error"] = "the build log would hold SMBIOS values; it was not written"
            os.remove(log_path)
        return validate.redact_obj(result, secrets)

    work = None
    try:
        report = _load_report(report_path)
        facts, stops = _precheck(report, rules, policy)
        result["facts"] = facts
        if stops:
            return finish("; ".join(stops))
        if not iasl and os.name == "nt":
            from . import sniff  # noqa: PLC0415
            iasl = sniff.windows_tool("iasl.exe", lock, cache)
        if not iasl or not os.path.isfile(iasl):
            return finish("no iasl: pass --iasl (on Windows the pinned ACPICA iasl.exe is used)")
        _prepare_out(out_dir)
        work = tempfile.mkdtemp(prefix="work-", dir=os.path.join(cache.root))
        tempfile.tempdir = os.path.join(work, "tmp")
        os.makedirs(tempfile.tempdir)
        ocs = upstream.copy_for_build("OpCore-Simplify", work, cache_root)
        ock = os.path.join(work, "OCK_Files")   # fresh per build: products are unpacked again from the checked zips
        os.makedirs(ock)
        tools = validate.Tools(lock, cache)
        # only the tables OpCore Simplify reads go into the build (an MSDM table holds the Windows product key)
        from . import sniff  # noqa: PLC0415
        tables = sniff.keep_build_tables(acpi_dir, os.path.join(work, "ACPI"))
        result["acpi_tables"] = tables
        acpi_dir = os.path.join(work, "ACPI")
        if not any(open(os.path.join(acpi_dir, t), "rb").read(4) == b"DSDT" for t in tables):
            return finish("no DSDT among the ACPI tables in {}".format(acpi_dir))

        tee = Tee(echo)
        a = Answerer(policy, {"report": os.path.abspath(report_path), "acpi": acpi_dir}, facts, tee)
        sys.path.insert(0, ocs)
        cwd = os.getcwd()
        error = None
        patches = None
        try:
            with contextlib.redirect_stdout(tee):
                os.chdir(work)
                patches = Patches(ocs, a, lock, cache, ock, iasl, tools.path, rules, lock_mode, log)
                patches.install()
                spec = importlib.util.spec_from_file_location("ocs_main", os.path.join(ocs, "OpCore-Simplify.py"))
                mod = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(mod)
                o = mod.OCPE()
                o.result_dir = os.path.join(work, "Results")
                o.main()
        except Answerer.Done:
            pass
        except answers.Stop as e:
            error = str(e)
        except pins.PinError as e:
            error = "download: {}".format(e)
        except Exception as e:  # the program's own errors: name them, keep the log
            import traceback  # noqa: PLC0415
            error = "OpCore Simplify failed: {}: {}\n{}".format(type(e).__name__, e, traceback.format_exc()[-3000:])
        finally:
            os.chdir(cwd)
            sys.path.remove(ocs)
        log.write(tee.text())
        result["decisions"] = a.decisions
        result["notices"] = a.notices
        if patches:
            result["downloads"] = patches.downloads
            if lock_mode:
                from . import lock as lock_mod  # noqa: PLC0415
                lock_mod.record(lock, patches.used_products, cache)
        if error:
            return finish(error)
        built = os.path.join(work, "Results", "EFI")
        if not os.path.isdir(built):
            return finish("OpCore Simplify finished without an EFI folder")
        shutil.copytree(built, os.path.join(out_dir, "EFI"))
        config = os.path.join(out_dir, "EFI", "OC", "config.plist")
        secrets = validate.read_secrets(config)

        # the install guide's stage-1 rules, then stage 2 from the shared rules
        result["changes"] += policy_mod.apply(config, report, facts, policy, rules)
        which = "settings-safari" if policy.safari else "settings"
        ops, lines, notes = rules_mod.apply_plan(config, which, rules)
        result["changes"] += [{"step": "Safari setting" if "SecureBootModel" in l else "stage 2",
                               "change": re.sub(r"^config\s+", "", l.strip())} for l in lines]
        result["changes"] += [{"step": "stage 2", "note": n} for n in notes]
        result["stage2"] = {"plan": which, "ops": len(ops), "notes": notes}

        result["firmware"] = policy_mod.firmware_steps(report, facts)
        result["after_install"] = policy_mod.after_install_notes(config)

        # checks: ocvalidate from the same OpenCore release, stage 2 complete, stage 4 not yet and possible
        result["checks"] = validate.validate(out_dir, cache_root=cache.root, report=report, policy=policy, secrets=secrets)
        if not result["checks"]["ok"]:
            return finish("the checks failed: " + "; ".join(result["checks"]["failures"]))
        result["ok"] = True
        return finish()
    except (pins.PinError, rules_mod.RulesError, validate.ValidatorUnavailable, OSError, ValueError) as e:
        return finish("{}: {}".format(type(e).__name__, e))
    finally:
        tempfile.tempdir = None
        if work and not os.environ.get("INTELXE_EFIBUILD_KEEP_WORK"):
            shutil.rmtree(work, ignore_errors=True)
