#!/usr/bin/env python3
"""Lock original-style pre recovery and same-callback root-companion logging."""
from pathlib import Path
import re

source = Path("module/src/main/cpp/main.cpp").read_text(encoding="utf-8")
pre = re.search(r"void preAppSpecialize\(.*?(?=\n    void postAppSpecialize)", source, re.S)
post = re.search(r"void postAppSpecialize\(.*?(?=\nprivate:)", source, re.S)
if not pre or not post:
    raise SystemExit("FAIL: specialization callbacks could not be located")
for required in (
    "vm_->GetEnv", "GetStringUTFChars", "ReleaseStringUTFChars", "args->uid",
    "FindRuleByPackageAndUid", "InspectOrRestoreLibrary", "CaptureArtifactThroughCompanion",
    "WriteLogThroughCompanion",
):
    if required not in pre.group(0):
        raise SystemExit(f"FAIL: missing {required} in preAppSpecialize")
for forbidden in ("exemptFd",):
    if forbidden in pre.group(0):
        raise SystemExit(f"FAIL: obsolete cross-stage FD dependency {forbidden} in preAppSpecialize")
if "InspectOrRestoreLibrary" in post.group(0) or "WriteLogThroughCompanion" in post.group(0):
    raise SystemExit("FAIL: recovery or logging must not run in postAppSpecialize")
for forbidden in ("GetStringUTFChars", "GetEnv", "AttachCurrentThread", "SendCompanionLog"):
    if forbidden in post.group(0):
        raise SystemExit(f"FAIL: forbidden {forbidden} in postAppSpecialize")
for required in ("REGISTER_ZYGISK_COMPANION(CompanionHandler)", "WriteLogThroughCompanion", "SendModuleDirFd", "SendCompanionLog"):
    if required not in source:
        raise SystemExit(f"FAIL: missing root-companion protocol component {required}")
print("PASS: package+UID recovery and synchronous root-companion logging are confined to preAppSpecialize")
