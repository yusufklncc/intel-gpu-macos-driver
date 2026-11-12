Xe probe stick
==============

This stick collects what the Intel GPU macOS driver project needs to bring up an Intel GPU: what Linux's own xe/i915
driver finds and programs on your hardware, and a register trace (mmiotrace) of that driver starting your GPU. It does
not install anything, has no network drivers, and sets every disk in the computer read-only before it mounts anything.

1. Turn Secure Boot off in the firmware setup (the stick's boot loader is not signed). Turn it back on afterwards.
2. Boot from this stick (the boot menu key is usually F8, F11 or F12). Choose "scan this computer".
3. Wait. It takes three to five minutes. The screen may go black or flicker while the GPU driver starts; that is
   expected. The computer turns itself off at the end.
4. Put the stick into any computer. The report is in REPORTS/: xe-stick-XR-XXXXXXXX.tar.gz, a .txt summary next to it
   and a .sha256 file. Attach the .tar.gz to a GitHub issue and write the report ID (XR-...) in the form.

If the computer freezes: hold the power button and boot the stick again. The stick remembers the step that froze this
computer and leaves it out from then on (REPORTS/xe-stick-state.txt; delete that file to try every step again).
If it freezes again, choose "safe scan": it starts no GPU driver at all.

What is collected, what never is, and why: tools/probe-stick/README.md in the project's repository.
The licences of what is on this stick: THIRD-PARTY-NOTICES.txt and LICENSES/.
