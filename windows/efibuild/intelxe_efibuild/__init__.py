# SPDX-License-Identifier: MIT
"""intelxe_efibuild: builds an OpenCore EFI from a Hardware Sniffer report without questions.

It runs OpCore Simplify (BSD-3-Clause, fetched at the commit in upstream/PINNED.json) with every question answered by
a recorded rule (answers.py), downloads only what downloads.lock.json pins, applies stage 2 of the install guide from
release/templates/efi-rules.json (rules.py) and checks the result with ocvalidate from the pinned OpenCore release
(validate.py). docs/design/DESIGN-efi-build.md describes it.
"""

FORMAT_VERSION = 1
