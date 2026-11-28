#!/usr/bin/env python3
"""Run the lab Hermes probe unit tests."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    return subprocess.call(
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            str(ROOT / "tools" / "hermes-probe" / "tests"),
            "-v",
        ]
    )


if __name__ == "__main__":
    raise SystemExit(main())
