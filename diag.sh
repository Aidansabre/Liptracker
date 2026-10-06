#!/bin/sh
# Modified 2026: Focus 3 USB/UVC diagnostics. Does not activate the IR or stream.
# Stop a running vft-stream service before collecting userspace descriptors.
set -u
dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
report=${1:-"$dir/diag.txt"}
{
  printf '== identity/kernel ==\n'
  id
  uname -a
  printf '\n== HTC USB descriptors ==\n'
  if command -v lsusb >/dev/null 2>&1; then
    lsusb -d 0bb4: -v
    lsusb -t
  else
    printf 'lsusb unavailable; use the binary diagnostics below.\n'
  fi
  printf '\n== HTC sysfs devices ==\n'
  for device in /sys/bus/usb/devices/*; do
    [ "$(cat "$device/idVendor" 2>/dev/null)" = 0bb4 ] || continue
    printf '%s: pid=%s product=%s\n' "$device" "$(cat "$device/idProduct")" "$(cat "$device/product" 2>/dev/null)"
    for interface in "$device":*; do
      [ -d "$interface" ] || continue
      printf '  %s class=%s driver=%s\n' "$interface" "$(cat "$interface/bInterfaceClass" 2>/dev/null)" "$(readlink "$interface/driver" 2>/dev/null)"
    done
  done
  printf '\n== userspace UVC descriptors (read-only) ==\n'
  if [ -x "$dir/vft-stream" ]; then
    "$dir/vft-stream" --tracker focus3 --diagnose
  else
    printf 'Put the ARM64 binary named vft-stream next to diag.sh, then rerun.\n'
  fi
} > "$report" 2>&1
printf 'Wrote %s (use sudo sh diag.sh if USB access is denied).\n' "$report"
