#!/usr/bin/env python3
"""Packages a built firmware.bin into the layout ota_update.h expects a
firmware-update host to serve, so it can be copied onto whatever's actually
hosting it -- a Synology NAS's Web Station folder, a GitHub Release, an S3/R2
bucket, whatever kOtaUpdateBaseUrl currently points at. Deliberately doesn't
touch the host itself (no NAS/SSH/API creds baked in here) -- it just
produces the exact 3 files, ready to drag/copy/rsync wherever they need to go.

Usage (from the project root, after building):
    pio run -e esp32-s3-knob
    python3 tools/package_ota_release.py 0.4.1

Reads version.h to sanity-check that the version you're publishing actually
matches what's baked into the binary you just built (a mismatch here means
you built before bumping version.h, or forgot to rebuild after bumping it --
both are easy to do and would otherwise silently publish the wrong pairing).

Produces, under dist/ota_release/<version>/:
    latest.txt                    -- one line, just the version string
    firmware-<version>.bin        -- copy of the built binary
    firmware-<version>.bin.sha256 -- sha256sum output for that file

Copy all 3 into your host's serving folder (e.g. the NAS's Web Station "ota"
folder) to publish. latest.txt is meant to be overwritten by each new
release (it's always just "the newest version"); the firmware-*.bin/.sha256
pair for each version can be left in place indefinitely so a board that
somehow missed a release can still be pointed at an older one manually.
"""
import hashlib
import re
import shutil
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_BIN = PROJECT_ROOT / ".pio" / "build" / "esp32-s3-knob" / "firmware.bin"
VERSION_H = PROJECT_ROOT / "src" / "version.h"


def read_version_h() -> str:
    text = VERSION_H.read_text()
    m = re.search(r'kVersion\s*=\s*"([^"]+)"', text)
    if not m:
        sys.exit(f"error: couldn't find kVersion in {VERSION_H}")
    return m.group(1)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <version>  (e.g. 0.4.1)")
    version = sys.argv[1]

    baked_in_version = read_version_h()
    if baked_in_version != version:
        sys.exit(
            f"error: version.h currently says \"{baked_in_version}\", but you're "
            f"publishing \"{version}\". Bump version.h, rebuild "
            f"(pio run -e esp32-s3-knob), then re-run this script -- publishing a "
            f"binary under a version string it doesn't actually match would make "
            f"every future check compare against the wrong number."
        )

    if not BUILD_BIN.exists():
        sys.exit(
            f"error: {BUILD_BIN} doesn't exist yet -- run "
            f"`pio run -e esp32-s3-knob` first (this script publishes whatever "
            f"that build produced, it doesn't build anything itself)."
        )

    out_dir = PROJECT_ROOT / "dist" / "ota_release" / version
    out_dir.mkdir(parents=True, exist_ok=True)

    bin_name = f"firmware-{version}.bin"
    bin_path = out_dir / bin_name
    shutil.copyfile(BUILD_BIN, bin_path)

    digest = hashlib.sha256(bin_path.read_bytes()).hexdigest()
    sha_path = out_dir / f"{bin_name}.sha256"
    # Same format `sha256sum` itself produces -- HTTPUpdate's sidecar parser
    # (see ota_update.cpp) just scans for the first bare 64-hex-char token,
    # so this exact two-column format isn't required, but matching it means
    # `sha256sum -c` also works if you ever want to check it by hand.
    sha_path.write_text(f"{digest}  {bin_name}\n")

    latest_path = out_dir / "latest.txt"
    latest_path.write_text(f"{version}\n")

    size_kb = bin_path.stat().st_size / 1024
    print(f"Packaged v{version} ({size_kb:.0f} KB) -> {out_dir}")
    print(f"  {latest_path.name}")
    print(f"  {bin_path.name}")
    print(f"  {sha_path.name}")
    print()
    print("Copy all 3 files into your OTA host's serving folder now (overwriting")
    print("any previous latest.txt) -- e.g. the Synology Web Station \"ota\" folder.")
    print(f"SHA-256: {digest}")


if __name__ == "__main__":
    main()
