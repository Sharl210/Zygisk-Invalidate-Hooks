# Inline Hook Spoof V2.9.2

V2 is a Zygisk module for restoring configured libraries' executable segments in selected application processes. The original project restored one globally selected library for a list of packages; V2 moves that library choice into each selected application's rule, so one application cannot inherit another application's library configuration.

## Per-application configuration

The active configuration remains in the module directory:

```text
/data/adb/modules/inline_hook_spoof/config.txt
```

A fresh installation starts with no selected application:

```ini
# Inline Hook Spoof per-application configuration
version=3
enabled=false
```

The WebUI creates independent application rules:

```ini
version=3
enabled=true
app=com.example.target|libart.so,libtarget_extra.so|true|11010
```

Each `app=` line has four fields separated by `|`:

1. **Package name**;
2. **Libraries to restore** — use English commas `,` or Chinese commas `，`; an empty list defaults to `libart.so`;
3. **Detailed log switch** — `true` or `false`, default `false`;
4. **UID** — automatically resolved by the WebUI and used only for pre-specialization matching; users do not need to enter it.

Examples:

```ini
# Default library only; no file log
app=com.example.alpha|libart.so|false|11010

# Multiple libraries and detailed logging
app=com.example.beta|libart.so，libbeta_extra.so,/apex/example/lib64/libguard.so|true|11011
```

The previous configuration remains readable for migration:

```text
1:libart.so
com.example.legacy
```

Its package entries are converted in memory to individual rules using the original shared library. Saving from the V2 WebUI writes the V3 per-application format.

## Scope isolation

- A package instance must have an explicit rule before its configured libraries are looked up.
- Rules use **package name + UID** as a composite key: the same package under different Android users/clone UIDs can have independent libraries and logs.
- Applications sharing one UID are selected and removed as a group in the WebUI; each package still receives its own rule and package-specific log directory.
- Unselected package/UID instances do not read target ELF files, do not modify memory, and do not create logs.
- Matching in pre-specialization requires both exact package name and UID; `com.example.app` does not automatically include `com.example.app:worker`.
- Removing an app rule affects its next process start. Any already-running process should be stopped or restarted once to discard its existing private memory state.
- Removing a rule never deletes historical logs.

## Detailed per-application logs

Logging is disabled by default for every app. Enable it in that app's configuration dialog. Each library attempt completes in pre-specialization: the target process synchronously connects to the root Zygisk companion, transfers the module-directory FD, sends its structured event, waits for the root write acknowledgement, and closes the socket before specialization continues. Logs are stored at `logs/<package>_<uid>/`; post-specialization does not write or deliver logs.

Each configured-library attempt creates or appends to:

```text
/data/adb/modules/inline_hook_spoof/logs/<package>_<uid>/YYYYMMDD-HHMMSS.log
```

The log contains:

- event time, ABI, package, process, rule library list;
- requested library, resolved path, load base, file size, device, inode, program-header count;
- every executable `PT_LOAD` segment's memory address, file offset, page range, and original protection;
- difference count, first/last differing offset, first-difference memory address;
- limited disk-versus-memory hex previews at the first difference;
- write attempt state, `USED_RWX_WRITE`, post-write verification, permission restoration, rollback and error details.

This gives a later analysis workflow a stable `(library path, file offset, memory address, first-difference bytes)` anchor for disassembly and detection-point investigation.

## Difference SO artifacts

When detailed logging is enabled and `TOTAL_DIFFERING_BYTES` is nonzero, the root companion publishes:

```text
/data/adb/modules/inline_hook_spoof/artifacts/<package>_<uid>/<timestamp>_<library>/
```

```text
disk-original.so  disk source used as the restoration baseline
before.so         reconstructed executable image before restoration
after.so          reconstructed executable image after restoration
diff-ranges.txt   every continuous changed range, file offset, memory address, and byte previews
manifest.txt      package, UID, source library, artifact path, and restore metadata
```

The artifact is created only after restoration has completed. `before.so` and `after.so` start from the same disk SO and receive the captured pre/post executable-segment overlays at their original file offsets. If no difference exists, the log records `ARTIFACT_STATUS=not_required_no_difference` and no large artifact files are created.

## WebUI

The original application list, running-app grouping, top-bar search, refresh action, navigation, and memory-map utility remain available. V2 changes configuration interaction:

- clicking an unconfigured app adds a rule with `libart.so` and logging disabled;
- clicking a configured app removes its rule;
- **Configure** opens that app's library list and log switch;
- typing, Enter, and refresh all apply application search;
- the former global library settings are replaced with an informational Settings page;
- the UI follows `navigator.language`: Chinese systems receive Simplified Chinese, other systems receive English;
- the top of the Home page shows a dedicated **Configured apps** section, with each package+UID instance's libraries and log state;
- **Advanced memory maps** opens the selected process's `/proc/<pid>/maps` diagnostic view. Confirmed exports go to `maps/<package>_<uid>/`, retain exact byte-range metadata and SHA-256, reject zero-filled or short reads, and compare the running memory range against the mapped file at the same file offset.

## Android 16 / LSP recovery timing

The original module works on the user's Android 16/LSP environment, so V2.9.2 restores its proven pre-specialization ordering instead of treating package-name JNI as a universally invalid path. The prior V2 crashes are treated as regressions from V2 behavior changes: post-stage full restoration could erase LSP trampolines, while RW-first writes could temporarily remove execution permission.

V2.9.2 uses package name and UID together:

```text
preAppSpecialize
  original-style nice_name read through Zygisk JNI path
  exact package-name + args->uid rule match
  complete configured-library restoration through RWX
  synchronous root-companion log write to logs/<package>_<uid>

postAppSpecialize
  no restoration and no log delivery
```

The WebUI automatically stores each selected App instance's UID as the fourth `app=` field. Same-package different-UID clones remain independent; packages sharing one UID are selected as a group. Detailed logs include:

```text
RECOVERY_STAGE=preAppSpecialize_package_uid_root_companion
```

The production writer retains `PROT_EXEC` through `RWX`, matching the original module's write-page behavior. This is source-level, host-level, four-ABI, and ZIP verified; actual Android 16/LSP device validation remains required.

## Internal recovery behavior

For every configured library in a selected process, V2 preserves complete executable `PT_LOAD + PF_X` segment restoration after the library resolves successfully. It adds:

1. Per-process configuration snapshots, preventing state leakage between applications;
2. complete retry-aware file reads;
3. ELF metadata, segment-boundary, file-range, and address-overflow validation;
4. complete-segment restoration compatible with the original behavior;
5. original-compatible `RWX` writes that retain executable permission during restoration, followed by original page-protection restoration;
6. original page-permission restoration and instruction-cache synchronization;
7. post-write verification and rollback attempts after partial failure;
8. four compiled ABI variants: `armeabi-v7a`, `arm64-v8a`, `x86`, and `x86_64`;
9. repaired installer scripts and payload SHA-256 checks.

V2 does not start a background service, push files through ADB, install itself automatically, restart zygote, or reboot the device.

## Build and validation

`tools/build-v2-manual.sh` builds all four Android native libraries with the official NDK r28b target sysroot. `tools/package-v2.sh` generates the install ZIP. Tests cover legacy and V3 configuration parsing, multiple libraries with Chinese/English commas, exact application scope, detailed timestamp log fields, segment recovery evidence, WebUI interaction, installer shell syntax, four ABI ELF validation, and ZIP integrity.
