# shellcheck disable=SC2034
SKIPUNZIP=1

DEBUG=@DEBUG@
SONAME=@SONAME@
SUPPORTED_ABIS="@SUPPORTED_ABIS@"

if [ "$BOOTMODE" ] && [ "$KSU" ]; then
  ui_print "- Installing from KernelSU app"
  ui_print "- KernelSU version: $KSU_KERNEL_VER_CODE (kernel) + $KSU_VER_CODE (ksud)"
  if [ "$(which magisk)" ]; then
    ui_print "*********************************************************"
    ui_print "! Multiple root implementations are not supported"
    ui_print "! Remove the conflicting root implementation before installing this module"
    abort    "*********************************************************"
  fi
elif [ "$BOOTMODE" ] && [ "$MAGISK_VER_CODE" ]; then
  ui_print "- Installing from Magisk app"
else
  ui_print "*********************************************************"
  ui_print "! Recovery installation is not supported"
  ui_print "! Install from the KernelSU or Magisk application"
  abort    "*********************************************************"
fi

VERSION=$(grep_prop version "${TMPDIR}/module.prop")
ui_print "- Installing $SONAME $VERSION"

support=false
for abi in $SUPPORTED_ABIS
do
  if [ "$ARCH" = "$abi" ]; then
    support=true
  fi
done
[ "$support" = true ] || abort "! Unsupported platform: $ARCH"
ui_print "- Device platform: $ARCH"

ui_print "- Extracting verifier"
VERIFY_EXPECTED=$(unzip -p "$ZIPFILE" 'verify.sh.sha256' 2>/dev/null | tr -d '\r\n')
case "$VERIFY_EXPECTED" in
  *[!0123456789abcdef]*|'') abort "! Missing or invalid verify.sh checksum" ;;
esac
[ ${#VERIFY_EXPECTED} -eq 64 ] || abort "! Invalid verify.sh checksum length"
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >&2
if [ ! -f "$TMPDIR/verify.sh" ]; then
  ui_print "*********************************************************"
  ui_print "! Unable to extract verify.sh"
  abort    "*********************************************************"
fi
VERIFY_ACTUAL=$(sha256sum "$TMPDIR/verify.sh" | awk '{print $1}')
[ "$VERIFY_ACTUAL" = "$VERIFY_EXPECTED" ] || abort "! verify.sh integrity check failed"
. "$TMPDIR/verify.sh"
extract "$ZIPFILE" 'customize.sh'  "$TMPDIR/.vunzip"
extract "$ZIPFILE" 'verify.sh'     "$TMPDIR/.vunzip"
extract "$ZIPFILE" 'sepolicy.rule' "$TMPDIR"

ui_print "- Extracting module files"
extract "$ZIPFILE" 'module.prop'     "$MODPATH"
extract "$ZIPFILE" 'post-fs-data.sh' "$MODPATH"
extract "$ZIPFILE" 'service.sh'      "$MODPATH"
mkdir -p "$MODPATH/webroot"
extract "$ZIPFILE" 'webroot/index.html' "$MODPATH/webroot" true
mv "$TMPDIR/sepolicy.rule" "$MODPATH"

# A fresh installation starts with no application rules. Each selected app gets
# its own default libart.so rule from the WebUI when it is added.
if [ ! -f "$MODPATH/config.txt" ]; then
  ui_print "- Creating per-application default config"
  cat > "$MODPATH/config.txt" <<'EOF'
# Inline Hook Spoof per-application configuration
version=3
enabled=false
EOF
fi

HAS32BIT=false
if [ -n "$(getprop ro.product.cpu.abilist32)" ] || [ -n "$(getprop ro.system.product.cpu.abilist32)" ]; then
  HAS32BIT=true
fi
mkdir -p "$MODPATH/zygisk"

extract_native() {
  native_abi=$1
  destination_name=$2
  ui_print "- Extracting $native_abi library"
  extract "$ZIPFILE" "lib/$native_abi/lib$SONAME.so" "$MODPATH/zygisk" true
  mv "$MODPATH/zygisk/lib$SONAME.so" "$MODPATH/zygisk/$destination_name"
}

# Keep the original module's four-ABI coverage, while handling true 32-bit
# devices explicitly instead of always extracting a 64-bit companion library.
case "$ARCH" in
  arm64)
    [ "$HAS32BIT" = true ] && extract_native "armeabi-v7a" "armeabi-v7a.so"
    extract_native "arm64-v8a" "arm64-v8a.so"
    ;;
  arm)
    extract_native "armeabi-v7a" "armeabi-v7a.so"
    ;;
  x64)
    [ "$HAS32BIT" = true ] && extract_native "x86" "x86.so"
    extract_native "x86_64" "x86_64.so"
    ;;
  x86)
    extract_native "x86" "x86.so"
    ;;
  *)
    abort "! Unsupported platform: $ARCH"
    ;;
esac

ui_print "- Setting permissions"
set_perm_recursive "$MODPATH" 0 0 0755 0644
