# SPDX-License-Identifier: MIT
"""The answer rules without OpCore Simplify: what stops a build and what each rule picks from a menu.

  python3 -I tests/test_answers.py
"""
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from intelxe_efibuild import answers  # noqa: E402
from intelxe_efibuild.engine import Answerer, Policy, Tee  # noqa: E402

ARC = "Intel(R) Arc(TM) B580 Graphics"


def make(macos="26", cards=(ARC,)):
    tee = Tee()
    a = Answerer(Policy(macos=macos), {"report": "/r/Report.json", "acpi": "/r/ACPI"}, {"cards": list(cards)}, tee)
    return a, tee


def ask(a, tee, screen, printed, prompt):
    a.head(screen)
    tee.write(printed + "\n")
    return a.ask(prompt)


class Rules(unittest.TestCase):
    def setUp(self):   # the answers are printed into the build's transcript; not here
        self._stdout, sys.stdout = sys.stdout, io.StringIO()

    def tearDown(self):
        sys.stdout = self._stdout

    def test_unknown_question_stops(self):
        a, t = make()
        with self.assertRaises(answers.Stop) as e:
            ask(a, t, "Something New", "1. yes\n2. no", "Enable the new thing? (yes/No): ")
        self.assertIn("no rule answers", str(e.exception))

    def test_unexpected_notice_stops(self):
        a, t = make()
        with self.assertRaises(answers.Stop):
            ask(a, t, "Building OpenCore EFI", "Unable to download Hyper Threading Patches at this time", "Press Enter to continue...")

    def test_refused_answer_stops(self):
        a, t = make()
        menu = "   24. macOS Sequoia 15\n   25. macOS Tahoe 26\n"
        self.assertEqual(ask(a, t, "Select macOS Version", menu, "Please enter the macOS version you want to use (default: macOS Tahoe 26): "), "25")
        with self.assertRaises(answers.Stop):
            ask(a, t, "Select macOS Version", menu, "Please enter the macOS version you want to use (default: macOS Tahoe 26): ")

    def test_macos_15(self):
        a, t = make(macos="15")
        menu = "   23. macOS Sonoma 14\n   24. macOS Sequoia 15\n   25. macOS Tahoe 26\n"
        self.assertEqual(ask(a, t, "Select macOS Version", menu, "Please enter the macOS version you want to use (default: macOS Tahoe 26): "), "24")
        self.assertIn("instead of the default 26", a.decisions[-1]["why"])

    def test_macos_not_offered_or_oclp_only(self):
        a, t = make()
        with self.assertRaises(answers.Stop):
            ask(a, t, "Select macOS Version", "   22. macOS Ventura 13\n   23. macOS Sonoma 14\n", "Please enter the macOS version (default: x): ")
        a, t = make()
        with self.assertRaises(answers.Stop):
            ask(a, t, "Select macOS Version", "   24. macOS Sequoia 15\n   25. macOS Tahoe 26 (Requires OpenCore Legacy Patcher)\n",
                "Please enter the macOS version (default: x): ")

    def test_gpu_combination_is_the_card_alone(self):
        a, t = make()
        menu = "1. {a}\n   Compatibility: x\n\n2. Intel(R) UHD Graphics 630\n\n3. Intel(R) UHD Graphics 630 + {a}\n".format(a=ARC)
        self.assertEqual(ask(a, t, "Hardware Customization", menu, "Select a GPU combination (1-3): "), "1")
        self.assertIn("UHD Graphics 630", a.decisions[-1]["why"])

    def test_intel_wifi_never_needs_oclp(self):
        a, t = make()
        menu = "1. laobamac AirportItlwm - Native WiFi\n2. AirportItlwm - Uses native WiFi settings menu\n3. itlwm - More stable overall\n"
        self.assertEqual(ask(a, t, "Select Required Kernel Extensions", menu, "Select Intel Wi-Fi kext (default: AirportItlwm-laobamac): "), "1")
        a, t = make()
        menu = "1. AirportItlwm - Uses native WiFi settings menu\n2. itlwm - More stable overall\n"
        self.assertEqual(ask(a, t, "Select Required Kernel Extensions", menu, "Select Intel Wi-Fi kext (default: itlwm): "), "2")

    def test_no_wifi_passwords(self):
        a, t = make()
        self.assertEqual(ask(a, t, "WiFi Profile Extractor", "", "Would you like to scan for WiFi profiles? (Yes/no): "), "no")

    def test_oclp_stops(self):
        a, t = make()
        with self.assertRaises(answers.Stop):
            ask(a, t, "OpenCore Legacy Patcher Warning", "", "Continue with OpenCore Legacy Patcher? (yes/No): ")

    def test_smbios_menu(self):
        a, t = make()
        compat = "Category: MacPro\n[*] 70. MacPro7,1        - W-3245M\n\nA. Show all models\nB. Back\n"
        self.assertEqual(ask(a, t, "Customize SMBIOS Model", compat, "Select your option: "), "a")
        full = "[ ] 64. MacBookPro16,2   - i5-1038NG7\n[*] 70. MacPro7,1        - W-3245M\nC. Show compatible models only\n"
        self.assertEqual(ask(a, t, "Customize SMBIOS Model", full, "Select your option: "), "64")
        done = "[*] 64. MacBookPro16,2   - i5-1038NG7\n[ ] 70. MacPro7,1        - W-3245M\n"
        self.assertEqual(ask(a, t, "Customize SMBIOS Model", done, "Select your option: "), "b")

    def test_main_menu_checks_the_choice(self):
        a, t = make()
        self.assertEqual(ask(a, t, None, "1. Select Hardware Report", "Select an option: "), "1")
        menu = "  macOS Version:   macOS Sequoia 15 (24.99.99)\n  SMBIOS:          MacPro7,1\n"
        with self.assertRaises(answers.Stop):
            ask(a, t, None, menu, "Select an option: ")

    def test_every_rule_is_in_the_table(self):
        for rid, *_ in answers.RULES:
            self.assertTrue(rid, rid)
        self.assertGreater(len(answers.QUESTION_TABLE.splitlines()), 15)


if __name__ == "__main__":
    unittest.main(verbosity=1)
