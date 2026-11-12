# SPDX-License-Identifier: MIT
"""Entry point: python -I efibuild.py COMMAND ... (see intelxe_efibuild/__main__.py). -I keeps the user's environment and
the current folder out of sys.path; this file puts only its own folder there."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from intelxe_efibuild.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
