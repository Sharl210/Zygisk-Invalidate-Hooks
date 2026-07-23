#!/usr/bin/env python3
"""Fail-closed structural verifier for the V2 module ZIP."""
from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

EXPECTED_ABIS = {
    "arm64-v8a": "AArch64",
    "armeabi-v7a": "ARM",
    "x86": "Intel 80386",
    "x86_64": "Advanced Micro Devices X86-64",
}
EXPECTED_NEEDED = {"libc.so", "libdl.so", "liblog.so", "libm.so"}
REQUIRED = {
    "module.prop",
    "customize.sh",
    "post-fs-data.sh",
    "service.sh",
    "verify.sh",
    "sepolicy.rule",
    "webroot/index.html",
    "README_V2.md",
    "META-INF/com/google/android/update-binary",
    "META-INF/com/google/android/updater-script",
}


def fail(message: str) -> None:
    raise AssertionError(message)


def readelf(*args: str) -> str:
    return subprocess.check_output(["readelf", *args], text=True, stderr=subprocess.STDOUT)


def main(zip_path: Path) -> None:
    if not zip_path.is_file():
        fail(f"package not found: {zip_path}")
    sidecar = zip_path.with_suffix(zip_path.suffix + ".sha256")
    if not sidecar.is_file():
        fail("external package SHA-256 sidecar is missing")
    actual_package_hash = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    declared_package_hash = sidecar.read_text(encoding="utf-8").split()[0]
    if actual_package_hash != declared_package_hash:
        fail("external package SHA-256 does not match")

    with zipfile.ZipFile(zip_path) as archive:
        broken = archive.testzip()
        if broken:
            fail(f"ZIP CRC failure: {broken}")
        files = {info.filename: archive.read(info.filename) for info in archive.infolist() if not info.is_dir()}

    missing = REQUIRED - files.keys()
    if missing:
        fail(f"missing required ZIP entries: {sorted(missing)}")
    if any(name.startswith("zygisk/") for name in files):
        fail("ZIP must contain pre-install lib/ entries, not a pre-created zygisk directory")

    for name, data in files.items():
        if name.endswith(".sha256"):
            continue
        checksum_name = f"{name}.sha256"
        if checksum_name not in files:
            fail(f"missing payload checksum: {checksum_name}")
        expected = files[checksum_name].decode("ascii").strip()
        actual = hashlib.sha256(data).hexdigest()
        if actual != expected:
            fail(f"payload checksum mismatch: {name}")

    prop = files["module.prop"].decode("utf-8")
    for line in ("id=inline_hook_spoof", "name=Inline Hook Spoof V2", "version=v2.9.2", "versionCode=29200"):
        if line not in prop:
            fail(f"module metadata missing: {line}")

    customize = files["customize.sh"].decode("utf-8")
    if re.search(r"@[A-Z_]+@", customize):
        fail("unrendered installer template token remains in packaged customize.sh")
    for token in (
        'SUPPORTED_ABIS="arm64 arm x86 x64"',
        'case "$ARCH" in',
        'arm64)',
        'arm)',
        'x64)',
        'x86)',
        'extract_native "arm64-v8a" "arm64-v8a.so"',
        'extract_native "armeabi-v7a" "armeabi-v7a.so"',
        'extract_native "x86_64" "x86_64.so"',
        'extract_native "x86" "x86.so"',
        'config.txt',
        'version=3',
        'enabled=false',
        'Creating per-application default config',
    ):
        if token not in customize:
            fail(f"safe-install or ABI-routing token missing: {token}")
    forbidden = re.compile(r"\b(killall\s+zygote|adb\s|reboot\b|magisk\s+--install|ksud\s+module\s+install)\b", re.I)
    for name in ("customize.sh", "post-fs-data.sh", "service.sh", "verify.sh"):
        if forbidden.search(files[name].decode("utf-8")):
            fail(f"forbidden device action in {name}")

    webui = files["webroot/index.html"].decode("utf-8")
    for token in (
        'id="search-input"',
        'id="per-app-libraries"',
        'id="per-app-log-enabled"',
        'id="per-app-dialog"',
        'id="configured-apps-section"',
        'id="configured-app-list"',
        'maps/${instance}',
        'iflag=skip_bytes,count_bytes',
        'NONZERO=',
        'DISK_COMPARE=',
        'FIRST_MEMORY_DIFFERENCE_ADDRESS=',
        'cmp -s',
        'sha256sum',
        'CONFIG_PATH = `/data/adb/modules/${MODULE_ID}/config.txt`',
        'version=3',
        'function normalizeLibraries(value)',
        'async function resolveUid(app)',
        "event.key === 'Enter'",
        'YYYYMMDD-HHMMSS.log',
    ):
        if token not in webui:
            fail(f"WebUI search contract missing: {token}")

    with tempfile.TemporaryDirectory(prefix="inline-hook-spoof-v2-verify-") as temp:
        root = Path(temp)
        for abi, expected_machine in EXPECTED_ABIS.items():
            entry = f"lib/{abi}/libinline_hook_spoof.so"
            if entry not in files:
                fail(f"missing native library: {entry}")
            library = root / abi / "libinline_hook_spoof.so"
            library.parent.mkdir(parents=True)
            library.write_bytes(files[entry])
            header = readelf("-h", str(library))
            if expected_machine not in header:
                fail(f"unexpected ELF machine for {abi}")
            dynamic = readelf("-d", str(library))
            needed = set(re.findall(r"Shared library: \[([^]]+)\]", dynamic))
            if needed != EXPECTED_NEEDED:
                fail(f"unexpected dynamic dependencies for {abi}: {sorted(needed)}")
            symbols = readelf("-Ws", str(library))
            if not re.search(r"\bzygisk_module_entry\b", symbols):
                fail(f"missing Zygisk entry symbol for {abi}")
            if not re.search(r"\bzygisk_companion_entry\b", symbols):
                fail(f"missing Zygisk companion entry symbol for {abi}")

    print(f"PASS: V2 ZIP verified: {zip_path}")
    print(f"PASS: package SHA-256: {actual_package_hash}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <module.zip>", file=sys.stderr)
        sys.exit(2)
    main(Path(sys.argv[1]))
