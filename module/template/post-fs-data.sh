#!/system/bin/sh
# Preserve the original module's configuration contract. No boot service and no
# process mutation occur here; the Zygisk module reads config.txt per target process.
MODDIR=${0%/*}
[ -f "$MODDIR/config.txt" ] && chmod 0644 "$MODDIR/config.txt"
