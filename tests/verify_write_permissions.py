#!/usr/bin/env python3
"""Ensure Android production restores executable code through RWX, not RW."""
from pathlib import Path
import re
import sys

source = Path("module/src/main/cpp/mainCore.cxx").read_text(encoding="utf-8")
match = re.search(
    r"bool MakeRangeWritableExecutable\(.*?\{(.*?)\n\}",
    source,
    re.S,
)
if not match:
    raise SystemExit("FAIL: MakeRangeWritableExecutable not found")
body = match.group(1)
if "#if defined(INLINE_HOOK_SPOOF_HOST_TEST)" not in body or "#else" not in body:
    raise SystemExit("FAIL: host fixture and production branches must be explicit")
production = body.split("#else", 1)[1].split("#endif", 1)[0]
if "PROT_READ | PROT_WRITE | PROT_EXEC" not in production:
    raise SystemExit("FAIL: Android production write path does not retain PROT_EXEC")
if re.search(r"mprotect\([^\n]*PROT_READ \| PROT_WRITE\)(?! \| PROT_EXEC)", production):
    raise SystemExit("FAIL: Android production contains a non-executable RW write transition")
if "usedRwxWrite" not in source:
    raise SystemExit("FAIL: RWX write evidence field missing")
print("PASS: Android production restoration retains executable permission during writes")
