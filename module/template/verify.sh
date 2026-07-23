#!/system/bin/sh
# V2 payload verifier.  This file is sourced by customize.sh after that script
# validates this verifier's own checksum.

TMPDIR_FOR_VERIFY="${TMPDIR}/.inline_hook_spoof_v2_verify"
mkdir -p "$TMPDIR_FOR_VERIFY"

abort_verify() {
  ui_print "*********************************************************"
  ui_print "! $1"
  ui_print "! The module ZIP failed an integrity check"
  abort "*********************************************************"
}

# extract <zip> <entry> <destination-directory> [junk-paths]
# Verifies <entry>.sha256 before returning.  If junk-paths is true, the entry
# is flattened into destination-directory, matching Magisk module conventions.
extract() {
  zip_file=$1
  entry=$2
  destination=$3
  junk_paths=${4:-false}

  [ -n "$zip_file" ] && [ -n "$entry" ] && [ -n "$destination" ] || abort_verify "Invalid extraction request"
  mkdir -p "$destination" || abort_verify "Could not create extraction directory"

  if [ "$junk_paths" = true ]; then
    output_path="$destination/$(basename "$entry")"
    unzip_options="-oj"
  else
    output_path="$destination/$entry"
    unzip_options="-o"
  fi

  expected_hash=$(unzip -p "$zip_file" "$entry.sha256" 2>/dev/null | tr -d '\r\n')
  [ -n "$expected_hash" ] || abort_verify "Missing checksum for $entry"
  case "$expected_hash" in
    *[!0123456789abcdef]*) abort_verify "Invalid checksum format for $entry" ;;
  esac
  [ ${#expected_hash} -eq 64 ] || abort_verify "Invalid checksum length for $entry"

  unzip $unzip_options "$zip_file" "$entry" -d "$destination" >&2 || abort_verify "Could not extract $entry"
  [ -f "$output_path" ] || abort_verify "$entry was not extracted"

  actual_hash=$(sha256sum "$output_path" | awk '{print $1}')
  [ "$actual_hash" = "$expected_hash" ] || abort_verify "Checksum mismatch for $entry"
  ui_print "- Verified $entry"
}
