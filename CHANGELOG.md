# Changelog

## V2.9.2 — Difference SO evidence bundle

- Preserved the V2 verified/rollback recovery core; the unbuilt original-direct-read experiment was removed.
- When a logged library has real differences, captures complete pre/post executable-segment snapshots after restoration and sends them to the root companion.
- Root companion materializes `disk-original.so`, `before.so`, `after.so`, `diff-ranges.txt`, and `manifest.txt` in `artifacts/<package>_<uid>/<timestamp>_<library>/`.
- `diff-ranges.txt` is generated root-side from complete snapshots, so every continuous difference range is retained without consuming the ordinary log payload budget.
- Logs explicitly state artifact creation, absence, or failure.

## V2.9.1 — Runtime memory-to-file comparison

- Maps exports now compare the running process memory range with the mapped backing file at `MAP_OFFSET`.
- `.meta` adds `DISK_COMPARE`, `FIRST_FILE_DIFFERENCE_OFFSET`, and `FIRST_MEMORY_DIFFERENCE_ADDRESS`.
- The comparison is read-only: it does not change process memory, page permissions, or ART code.

## V2.9.0 — Root-synchronous evidence and package-UID artifacts

- Rebuilt file logging around the documented Zygisk root companion lifecycle: connect, module-directory FD transfer, event send, root write acknowledgement and close all happen in `preAppSpecialize`.
- Added UID pre-match diagnostics: `UID_PRE_MATCH`, `PACKAGE_PRE_MATCH`, actual `nice_name`, actual UID and `RESTORE_ATTEMPTED` distinguish a missed rule from a failed restore.
- Changed logs to `logs/<package>_<uid>/`.
- Added a top-level WebUI **Configured apps** section; configured instances no longer disappear into the unconfigured list.
- Replaced WebUI maps dumping to app-private storage with root export to `maps/<package>_<uid>/`, precise byte-offset reads, nonzero/short-read rejection, SHA-256 and `.meta` evidence.

## V2.8.0 — Same-callback direct log persistence

- Device evidence showed recovery worked but no files appeared after two companion/socket approaches.
- Removed companion IPC, socket transfer, FD exemption, and post-stage log delivery from the production path.
- After each pre-specialization restore attempt, the module directly writes the structured event through the `getModuleDir()` FD using `mkdirat`, `openat`, and `write`.
- `postAppSpecialize` is now intentionally empty, preventing both post-LSP ART writes and deferred log delivery dependencies.
- ZIP verification now rejects a companion entry symbol for the direct-log build.

## V2.7.0 — Companion log socket preservation

- Device evidence showed recovery no longer crashes but no log file appeared.
- Fixed the root companion socket lifecycle: after module-directory FD transfer, `api_->exemptFd(companionFd)` keeps the target-side socket open through Zygote specialization.
- Post-specialization can now send the captured recovery event to the companion rather than writing into a socket Zygote already closed.
- Added lifecycle verification requiring socket FD exemption before post log delivery.

## V2.6.0 — Original timing baseline and clone-aware rules

### Original behavior baseline

- User confirmed the original module runs on the affected Android 16/LSP device; package-name pre-specialization matching is restored as the baseline instead of being replaced by UID-only matching.
- Restoration now requires both original-style `nice_name` package equality and `args->uid` equality.
- Post-specialization remains log-only, so complete restoration does not erase LSP hooks after installation.

### Clone and shared UID support

- Rules are keyed by package name plus UID, allowing the same package under different clone/user UIDs to have separate libraries and logs.
- WebUI enumerates Android users and displays application instances with UID.
- Selecting or removing a package automatically selects/removes every visible package that shares the same UID.
- Shared UID entries retain independent package-specific rules and log directories.

## V2.5.0 — LSP ordering and original RWX write semantics

### LSP compatibility

- User confirmed LSP is actively hooking ART; post-specialization complete `libart.so` restoration could erase LSP trampoline bytes after LSP installed them.
- Added WebUI-resolved UID as the fourth per-app rule field and matches `args->uid` in pre-specialization without JNI.
- Restoration now occurs in the UID-matched pre callback, before LSP's later app hook setup; post-specialization only delivers logs and does not restore libraries.
- Added shared-UID protection: a UID shared by multiple installed packages remains inactive to avoid touching an unselected package.

### Page permission correction

- Reverted the V2 RW-first policy because it removed executable permission from ART pages during writes and directly matches `SEGV_ACCERR trying to execute non-executable memory`.
- Production Android writes now use the original project-compatible `RWX` transition and restore original permissions afterward.
- Added a static production-path test prohibiting a non-executable RW write transition.

## V2.3.0 — USAP JNI elimination

### Second Android 16 crash correction

- A second real `usap64` crash occurred in `libart.so::JNI::GetStringUTFLength`, proving that pre-specialization `GetStringUTFChars(args->nice_name)` remained unsafe on the affected Android 16 device.
- Removed all pre-specialization JNI, including process-name conversion, `GetEnv`, and thread attachment.
- `preAppSpecialize` now snapshots configuration only; `postAppSpecialize` reads `nice_name`, matches the application rule, restores libraries, and sends logs.
- Strengthened lifecycle regression tests to reject JNI process-name APIs in the pre callback.

## V2.2.0 — Android 16 USAP timing fix and localized diagnostics

### Crash correction

- Moved all configured-library lookup, ELF reads, page protection changes, restoration, verification, and log writing from `preAppSpecialize` to `postAppSpecialize`.
- `preAppSpecialize` now only snapshots the selected per-app rule and preserves the logging directory file descriptor when needed.
- Added a lifecycle regression test that prevents `InspectOrRestoreLibrary` or file-log writes from returning to the Zygote/USAP callback.
- Added a root Zygisk companion log writer: the post-specialization app process sends structured events over a Unix socket, while the companion receives the module-directory FD and writes logs with root directory access.
- Added `RECOVERY_STAGE=postAppSpecialize` to detailed logs.

### Interface and diagnostics

- Added automatic Simplified Chinese UI for `zh*` system languages and English fallback for other languages.
- Restored Settings as an informational page instead of a global library editor.
- Renamed Memory maps as an advanced diagnostic entry and added confirmation before exporting a selected memory region.

## V2.1.0 — Per-application libraries and evidence logs

### Per-application rules

- Replaced the global-library / package-list user model with independent `app=<package>|<libraries>|<log>` rules in `config.txt`.
- Each newly selected app defaults to `libart.so`.
- Added multiple-library support using English commas or Chinese commas.
- Added an independent `log=true|false` switch for each application; default is `false`.
- Preserved parsing support for the original `1:library` plus package-list format.
- Enforced exact package matching so unselected apps and child processes are not touched.

### Evidence logs

- Added optional logs at `logs/<package>/YYYYMMDD-HHMMSS.log` under the module directory.
- Logs include configured/resolved library identity, load base, file identity, executable segment metadata, difference distribution, bounded byte previews, write mode, verification, page-permission recovery, rollback state, and failures.
- Logs are append-only and are retained after a rule is removed.

### WebUI

- Retained the original app list, search, refresh, navigation, and diagnostic entry.
- Added per-app Configure dialogs for library lists and log switches.
- Clicking an unconfigured app creates the default per-app rule; clicking a configured app removes its rule.
- Continued support for input, Enter, and refresh-based application search.

### Existing V2 reliability work

- Per-process configuration snapshots and no state leakage between apps.
- Complete disk reads, ELF/range validation, complete executable-segment restoration, RW-first / RWX compatibility writes, cache synchronization, verification, and rollback attempts.
- Four rebuilt ABIs: `armeabi-v7a`, `arm64-v8a`, `x86`, `x86_64`.
- Repaired installer scripts and payload SHA-256 verification.
