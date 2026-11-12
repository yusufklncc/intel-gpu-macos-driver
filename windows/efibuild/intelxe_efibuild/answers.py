# SPDX-License-Identifier: MIT
"""The answers to OpCore Simplify's questions, one rule per question.

OpCore Simplify is a console program: every question goes through Utils.request_input. The engine replaces that function
with Answerer.ask, which looks the question up here by the screen it is on (the title of the last Utils.head) and the
prompt's text. A rule gives the answer and the reason, and both are recorded in the build log. A question that no rule
matches, or one asked again after it was answered (the program refused the answer), stops the build: guessing is how an
EFI ends up booting to a black screen. "Press Enter" prompts are notices; they are answered only on the screens where a
notice is expected, and a notice on any other screen (a failed download, tables that could not be read) stops the build.

Each rule is (id, screen regex, prompt regex, function). The function gets the Answerer and returns (answer, why), or
raises Stop. The table at the end is what docs/design/DESIGN-efi-build.md lists.
"""
import re

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class Stop(RuntimeError):
    """A build that cannot go on: an unanswered question, a refused answer, a rule that says no."""


def clean(text):
    return ANSI.sub("", text or "")


# ---- the main menu ---------------------------------------------------------------------------------------------------

def main_menu(a):
    """1 (read the report, which asks the rest), then 5 when the Mac model is not the policy's, then 6 (build); after
    the build the engine is done. The menu prints the chosen macOS and SMBIOS above it; both are checked."""
    text = a.context
    if not a.state.get("report_selected"):
        a.state["report_selected"] = True
        return "1", "select the hardware report; the program then asks for the macOS version, GPUs, kexts and tables"
    if a.state.get("built"):
        raise a.Done()
    macos = re.search(r"macOS Version:\s+(.+?)\s+\((\d+)\.\d+\.\d+\)", text)
    smbios = re.search(r"SMBIOS:\s+(\S+)", text)
    if not macos or macos.group(2) != a.policy.darwin:
        raise Stop("OpCore Simplify chose {} instead of {}".format(macos.group(1) if macos else "no macOS", a.policy.macos_name))
    if "Requires OpenCore Legacy Patcher" in text:
        raise Stop("this PC needs OpenCore Legacy Patcher root patches for {} (OpCore Simplify says so on its main menu). "
                   "The driver's stage 2 relaxes SIP and AMFI only as far as it needs; a system patched by OpenCore Legacy "
                   "Patcher is not a configuration it is tested with.".format(a.policy.macos_name))
    if smbios and smbios.group(1) != a.policy.smbios and not a.state.get("smbios_done"):
        a.state["smbios_done"] = True
        a.facts["smbios_offered"] = smbios.group(1)
        return "5", "OpCore Simplify chose {}; the policy's Mac model is {}".format(smbios.group(1), a.policy.smbios)
    if smbios and smbios.group(1) != a.policy.smbios:
        raise Stop("the Mac model is still {} after choosing {}".format(smbios.group(1), a.policy.smbios))
    a.state["build_started"] = True
    a.facts["smbios"] = smbios.group(1) if smbios else ""
    return "6", "build the EFI: macOS {}, {}".format(macos.group(1), smbios.group(1) if smbios else "?")


def report_path(a):
    if a.state.get("report_given"):
        raise Stop("OpCore Simplify did not accept the report:\n" + a.context[-2000:])
    a.state["report_given"] = True
    return a.inputs["report"], "the Hardware Sniffer report given to the builder"


def acpi_path(a):
    if a.state.get("acpi_given"):
        raise Stop("OpCore Simplify could not read the ACPI tables (it asks for them again):\n" + "\n".join(a.notices[-2:])[-2000:])
    a.state["acpi_given"] = True
    return a.inputs["acpi"], "the ACPI tables dumped with the report"


def macos_version(a):
    """The policy's macOS (26 by default, 15 on request), only when OpCore Simplify lists it without OCLP."""
    offered = {}
    for n, name, oclp in re.findall(r"^\s*(\d+)\. (macOS [^\n(]+?)\s*(\(Requires OpenCore Legacy Patcher\))?\s*$", a.context, re.M):
        offered[n] = (name, bool(oclp))
    want = a.policy.darwin
    if want not in offered:
        raise Stop("OpCore Simplify does not offer {} for this PC (it offers {}).".format(
            a.policy.macos_name, ", ".join(v[0] for v in offered.values()) or "nothing"))
    if offered[want][1]:
        raise Stop("{} needs OpenCore Legacy Patcher on this PC, which this builder does not do.".format(a.policy.macos_name))
    return want, "{} (Darwin {}): the policy's macOS{}".format(a.policy.macos_name, want,
                                                             "" if a.policy.macos == "26" else ", chosen instead of the default 26")


def build_for_uefi(a):
    return "yes", "OpenCore is built for UEFI boot; the firmware must boot in UEFI mode with CSM off (install guide, stage 1)"


def gpu_combination(a):
    """The combination that is exactly the supported Arc card(s): the installer runs on the card's firmware output, and
    an integrated GPU is turned off for macOS (the test machine ran without one)."""
    cards = set(a.facts.get("cards", []))
    choices = {}
    for n, line in re.findall(r"^(\d+)\. (.+)$", a.context, re.M):
        choices[n] = set(x.strip() for x in line.split(" + "))
    pick = [n for n, devices in choices.items() if devices == cards]
    if len(pick) != 1:
        raise Stop("no GPU combination of exactly {} among:\n{}".format(", ".join(sorted(cards)), a.context[-1500:]))
    others = sorted(set().union(*choices.values()) - cards)
    return pick[0], "{} alone{}".format(" + ".join(sorted(cards)), "; turned off for macOS: " + ", ".join(others) if others else "")


def first_device(a):
    """Wi-Fi or Bluetooth: macOS works best with one; the first one Hardware Sniffer listed."""
    kind = re.match(r"Select a (.+?) device", a.prompt).group(1)
    first = re.search(r"^1\. (.+)$", a.context, re.M)
    return "1", "the first {} device listed ({}); the others are turned off for macOS".format(kind, first.group(1).strip() if first else "?")


def tahoe_audio(a):
    if not re.search(r"^1\. AppleALC \+ SimpleHDA", a.context, re.M):
        raise Stop("the audio menu changed:\n" + a.context[-800:])
    return "1", "AppleALC + SimpleHDA: audio without OpenCore Legacy Patcher root patches (option 2 needs them)"


def broadcom_wifi(a):
    if not re.search(r"^1\. SimpleBCM", a.context, re.M):
        raise Stop("the Broadcom Wi-Fi menu changed:\n" + a.context[-800:])
    return "1", "SimpleBCM: native Wi-Fi without root patches (IOSkywalkFamily needs OpenCore Legacy Patcher)"


def intel_wifi(a):
    """The kext that needs no OCLP root patch: the laobamac AirportItlwm fork where it is offered, else itlwm.
    Plain AirportItlwm needs root patches on macOS 15 and later."""
    for name, why in (("laobamac AirportItlwm", "native Wi-Fi on macOS 15.2+ and 26 without root patches"),
                      ("itlwm", "no root patches (Wi-Fi through the HeliPort app)")):
        m = re.search(r"^(\d+)\. {}\b".format(re.escape(name)), a.context, re.M)
        if m:
            return m.group(1), "{}: {}".format(name, why)
    raise Stop("the Intel Wi-Fi menu lists neither the laobamac AirportItlwm nor itlwm:\n" + a.context[-800:])


def amd_gpu_kext(a):
    return "", "the default kext OpCore Simplify recommends for this AMD GPU"


def force_load(a):
    return "no", "kexts are not forced onto a macOS they do not support"


def codec_layout(a):
    m = re.search(r"default: (\d+)", a.prompt)
    return "", "the default layout{}".format(" ({})".format(m.group(1)) if m else "")


def wifi_profiles(a):
    return "no", "saved Wi-Fi passwords are never copied into the EFI"


def oclp_warning(a):
    raise Stop("OpCore Simplify says this PC needs OpenCore Legacy Patcher for {}; this builder does not build such an "
               "EFI (the driver is tested on an unpatched system).".format(a.policy.macos_name))


def smbios_menu(a):
    """Pick the policy's Mac model in OpCore Simplify's SMBIOS list: show all models when it is not among the
    compatible ones (a MacBookPro model on a desktop), select it, then go back."""
    want = a.policy.smbios
    rows = re.findall(r"^\[(\*| )\]\s*(\d+)\.\s+(\S+)", a.context, re.M)
    for mark, n, name in rows:
        if name == want:
            if mark == "*":
                return "b", "{} is selected".format(want)
            return n, "{}: {}".format(want, a.policy.smbios_why)
    if re.search(r"^A\. Show all models", a.context, re.M) and not a.state.get("smbios_all"):
        a.state["smbios_all"] = True
        return "a", "{} is not in the list of compatible models; show all".format(want)
    raise Stop("OpCore Simplify does not list the Mac model {}".format(want))


def notice(a):
    return "", "notice"


def stop_notice(why):
    def rule(a):
        raise Stop(why + ":\n" + a.context[-2000:])
    return rule


def result_screen(a):
    a.state["built"] = True
    return "", "the EFI is built"


# (id, screen, prompt, rule). Screens are the titles OpCore Simplify prints with Utils.head; first match wins.
RULES = [
    ("main-menu", r"^OpCore Simplify$", r"^Select an option:", main_menu),
    ("report", r"^Select hardware report$", r"^Drag and drop your hardware report", report_path),
    ("report-invalid", r"^(Select hardware report|Validation Report)$", r"^Press Enter to go back", stop_notice("the report did not pass OpCore Simplify's checks")),
    ("compatibility", r"^Compatibility Checker$", r"^Press Enter to continue", notice),
    ("macos", r"^Select macOS Version$", r"^Please enter the macOS version", macos_version),
    ("uefi", r"^Hardware Customization$", r"^Build EFI for UEFI\?", build_for_uefi),
    ("gpu", r"^Hardware Customization$", r"^Select a GPU combination \(1-\d+\)", gpu_combination),
    ("wifi-bt", r"^Hardware Customization$", r"^Select a (WiFi|Bluetooth) device \(1-\d+\)", first_device),
    ("devices", r"^Device Selection Summary$", r"^Press Enter to continue", notice),
    ("audio", r"^Select Required Kernel Extensions$", r"^Select audio kext for your system", tahoe_audio),
    ("broadcom-wifi", r"^Select Required Kernel Extensions$", r"^Select Broadcom Wi-Fi kext", broadcom_wifi),
    ("intel-wifi", r"^Select Required Kernel Extensions$", r"^Select Intel Wi-Fi kext", intel_wifi),
    ("amd-gpu", r"^Select Required Kernel Extensions$", r"^Select kext for your AMD .* GPU", amd_gpu_kext),
    ("kext-notice", r"^Select Required Kernel Extensions$", r"^Press Enter to continue", notice),
    ("force-load", r"^Kext Compatibility Check$", r"^Do you want to force load", force_load),
    ("acpi", r"^Select ACPI Tables$", r"^Please drag and drop ACPI Tables folder", acpi_path),
    ("acpi-failed", r"^Loading ACPI Table\(s\)$", r"^Press Enter", stop_notice("OpCore Simplify could not load the ACPI tables")),
    ("smbios", r"^Customize SMBIOS Model$", r"^Select your option:", smbios_menu),
    ("oclp", r"^OpenCore Legacy Patcher Warning$", r"^Continue with OpenCore Legacy Patcher\?", oclp_warning),
    ("codec", r"^Choosing Codec Layout ID$", r"^Enter the ID of the codec layout", codec_layout),
    ("wifi-profiles", r"^WiFi Profile Extractor$", r"^Would you like to scan for WiFi profiles", wifi_profiles),
    ("download-failed", r"^Gathering Files$", r"^Press Enter", stop_notice("a download failed")),
    ("before-using", r"^Before Using EFI$", r"^Press Enter", notice),
    ("result", r"^Result$", r"^Press Enter to main menu", result_screen),
]

# how often a question may come back in a row (the main menu and the SMBIOS list are asked once per step); any other
# question asked a second time in a row means the program refused the answer
REPEATS = {"main-menu": 6, "smbios": 4}

QUESTION_TABLE = """\
| screen | question | answer | why |
|---|---|---|---|
| main menu | Select an option | 1, then 5 if the Mac model differs, then 6; done after the build | read the report, set the model, build |
| Select hardware report | Drag and drop your hardware report | the Report.json given | asked once; again means it was refused: stop |
| Compatibility Checker | Press Enter | (notice) | the compatibility list goes into the log |
| Select macOS Version | Please enter the macOS version | 25 (macOS 26), or 24 (macOS 15) | stop if not offered, or offered only with OCLP |
| Hardware Customization | Build EFI for UEFI? | yes | UEFI boot, CSM off (guide, stage 1) |
| Hardware Customization | Select a GPU combination | the Arc card(s) alone | installer on the card's firmware output; iGPU off for macOS |
| Hardware Customization | Select a WiFi/Bluetooth device | 1 | the first listed; macOS wants one |
| Select Required Kernel Extensions | Select audio kext (macOS 26) | 1 (AppleALC + SimpleHDA) | no OCLP root patches |
| Select Required Kernel Extensions | Select Broadcom Wi-Fi kext | 1 (SimpleBCM) | no OCLP root patches |
| Select Required Kernel Extensions | Select Intel Wi-Fi kext | laobamac AirportItlwm, else itlwm | no OCLP root patches |
| Select Required Kernel Extensions | Select kext for your AMD GPU | default | (not reached with the Arc card alone) |
| Kext Compatibility Check | force load unsupported kexts? | no | nothing forced onto an unsupported macOS |
| Select ACPI Tables | drag and drop ACPI Tables folder | the ACPI folder given | asked again means unreadable: stop |
| Customize SMBIOS Model | Select your option | a (show all) if needed, the model's number, b | the policy's model (MacBookPro16,2) |
| OpenCore Legacy Patcher Warning | Continue with OCLP? | (stop) | not a configuration the driver is tested with |
| Choosing Codec Layout ID | Enter the codec layout ID | default | OpCore Simplify's default layout |
| WiFi Profile Extractor | scan for WiFi profiles? | no | no Wi-Fi passwords in the EFI |
| Gathering Files / Loading ACPI Table(s) / report check | Press Enter | (stop) | a failure notice |
| Before Using EFI, Device Selection Summary, kext notices | Press Enter | (notice) | recorded in the log |
| Result | Press Enter to main menu | (done) | the EFI is built |
"""
