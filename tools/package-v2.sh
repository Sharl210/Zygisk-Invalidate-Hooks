#!/usr/bin/env bash
# Package a tested V2 Magisk/KernelSU module ZIP from manually cross-compiled
# Android libraries. This script never pushes, installs, reboots, or restarts
# a connected device.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION_NAME=${VERSION_NAME:-v2.9.2}
VERSION_CODE=${VERSION_CODE:-29200}
STAGE="$ROOT/build/package-stage"
DIST="$ROOT/dist"
PACKAGE="$DIST/Inline-Hook-Spoof-V2-${VERSION_NAME}-${VERSION_CODE}.zip"

for abi in arm64-v8a armeabi-v7a x86 x86_64; do
  [[ -f "$ROOT/build/manual/$abi/libinline_hook_spoof.so" ]] || {
    echo "missing compiled library for $abi" >&2
    exit 1
  }
done
command -v zip >/dev/null
command -v unzip >/dev/null
command -v sha256sum >/dev/null

rm -rf "$STAGE" "$DIST"
mkdir -p "$STAGE" "$DIST"
chmod 0755 "$DIST"
cp -a "$ROOT/module/template/." "$STAGE/"
# Gradle normally performs these token substitutions.  The manual packager must
# render them before hashing, otherwise the installer would see literal tokens.
sed -i \
  -e 's/@DEBUG@/false/g' \
  -e 's/@SONAME@/inline_hook_spoof/g' \
  -e 's/@SUPPORTED_ABIS@/arm64 arm x86 x64/g' \
  "$STAGE/customize.sh" "$STAGE/post-fs-data.sh" "$STAGE/service.sh"
if grep -R -n -E '@[A-Z_]+@' "$STAGE"; then
  echo "unrendered installer template token detected" >&2
  exit 1
fi
mkdir -p "$STAGE/lib/arm64-v8a" "$STAGE/lib/armeabi-v7a" "$STAGE/lib/x86" "$STAGE/lib/x86_64"
for abi in arm64-v8a armeabi-v7a x86 x86_64; do
  cp "$ROOT/build/manual/$abi/libinline_hook_spoof.so" "$STAGE/lib/$abi/"
done

cat > "$STAGE/module.prop" <<EOF
id=inline_hook_spoof
name=Inline Hook Spoof V2
version=$VERSION_NAME
versionCode=$VERSION_CODE
author=Antik; V2 safety maintenance
summary=Per-application multi-library inline hook restoration with optional detailed logs.
description=Each selected app has independent libraries and optional recovery evidence logs.
EOF

cat > "$STAGE/README_V2.md" <<'EOF'
# Inline Hook Spoof V2

## Per-application configuration

The active configuration remains at:

```text
/data/adb/modules/inline_hook_spoof/config.txt
```

A fresh installation starts with no selected application:

```ini
# Inline Hook Spoof per-application configuration
version=3
enabled=false
```

The WebUI writes one independent rule per selected application:

```ini
version=3
enabled=true
app=com.example.target|libart.so,libtarget_extra.so|true|11010
```

Rule fields are:

1. Package name;
2. zero or more libraries, separated by English or Chinese commas;
3. optional detailed-log switch (`true` or `false`);
4. UID automatically resolved by the WebUI for pre-specialization matching.

An empty library field defaults to `libart.so`. Rules use package name plus
UID: same-package different-UID clones can be configured separately. Packages
sharing one UID are selected or removed together in the WebUI while retaining
separate package-specific rules and log directories. Unselected application
instances never enter library lookup, restoration, or per-app log creation.

## Detailed logs

When the rule log switch is enabled, every configured library attempt writes:

```text
/data/adb/modules/inline_hook_spoof/logs/<package>_<uid>/YYYYMMDD-HHMMSS.log
```

Logs are retained after a rule is removed. Each configured-library attempt
connects to the root companion and completes the FD transfer, structured event,
root write and acknowledgement **inside pre-specialization**, before the socket
closes. The root writer creates `logs/<package>_<uid>/`. No post-stage delivery
is used. They include resolved library path,
base, ELF segments, memory and file offsets, page permissions, difference
counts and previews, write mode, verification, rollback state, and errors.

## Difference SO artifacts

When a selected logged library has real differing bytes, the root companion creates:

```text
/data/adb/modules/inline_hook_spoof/artifacts/<package>_<uid>/<timestamp>_<library>/
```

The directory contains `disk-original.so`, reconstructed `before.so`, reconstructed
`after.so`, `diff-ranges.txt`, and `manifest.txt`. Ranges report file offsets,
memory addresses and byte previews. No large artifact files are generated when
there is no actual difference.

## Memory-map exports

The WebUI writes confirmed exports under:

```text
/data/adb/modules/inline_hook_spoof/maps/<package>_<uid>/
```

Each export has a `.bin` file plus `.meta` metadata. It uses exact byte offsets
from `/proc/<pid>/mem`, rejects short reads and all-zero content, compares the
memory range to the backing mapped file at the same `MAP_OFFSET`, and records
PID, map range, permissions, requested/actual/nonzero byte counts, SHA-256,
comparison state, and first difference location when available.

## Android 16 / LSP timing

V2.9.2 preserves the original project's pre-specialization package timing and
uses package+UID matching. The same pre callback restores through RWX, then
synchronously sends its structured event to the root companion and receives the
write result before specialization continues. The post callback is intentionally
empty: it neither restores libraries nor delivers logs. Same-package
different-UID clones stay independent; same-UID packages are selected together.
Logs include `RECOVERY_STAGE=preAppSpecialize_package_uid_root_companion`.

## V2 internal upgrades

V2 preserves complete executable-segment restoration after a selected process
and configured library resolve successfully. It adds complete disk reads,
ELF/range checks, original-compatible RWX permission writes that retain execution,
cache sync,
post-write verification, rollback attempts, repaired installer integrity
checks, and all four original Android ABI libraries.

No boot service, package push, automatic install, reboot, or zygote restart is
performed by this module.
EOF

# Hash every payload file. verify.sh validates extracted payloads before the
# installer copies them into the module directory.
while IFS= read -r -d '' file; do
  relative=${file#"$STAGE/"}
  sha256sum "$file" | awk '{print $1}' > "$STAGE/$relative.sha256"
done < <(find "$STAGE" -type f ! -name '*.sha256' -print0 | sort -z)

(
  cd "$STAGE"
  zip -X -q -r "$PACKAGE" .
)
unzip -t "$PACKAGE" >/dev/null
sha256sum "$PACKAGE" > "$PACKAGE.sha256"
chmod 0644 "$PACKAGE" "$PACKAGE.sha256"
printf 'PASS: %s\n' "$PACKAGE"
printf 'SHA-256: '
cut -d' ' -f1 "$PACKAGE.sha256"
