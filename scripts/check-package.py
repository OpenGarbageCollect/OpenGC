#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 OpenGC contributors
"""Install, relocate and consume both providers; all temporary artifacts are removed."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="opengc-package-") as directory:
        root = Path(directory)
        stage = root / "stage"
        subprocess.run(["cmake", "--install", str(args.build_dir.resolve()), "--prefix", str(stage)], check=True)
        relocated = root / "relocated"
        stage.rename(relocated)
        for provider in ("full", "retain"):
            build = root / provider
            subprocess.run(["cmake", "-S", str(source / "tests/consumer"), "-B", str(build), "-G", "Ninja",
                            f"-DCMAKE_PREFIX_PATH={relocated}", f"-DOPENGC_CONSUMER_PROVIDER={provider}"], check=True)
            subprocess.run(["cmake", "--build", str(build)], check=True)
            subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"], check=True)
    print("OpenGC install/relocation: full and retain consumers passed")


if __name__ == "__main__":
    main()
