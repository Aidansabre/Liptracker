#!/bin/sh
# Focus 3 capture experiments for Steam Frame. Run with sudo next to the
# vft-stream binary after unplugging/reconnecting the tracker once; it writes
# experiments.txt. Each variant runs for up to 15 s. Between variants the
# tracker is re-enumerated through sysfs so each starts from a fresh USB state.
set -u
dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
report=${1:-"$dir/experiments.txt"}
binary="$dir/vft-stream"
[ "$(id -u)" = 0 ] || { echo 'Run with sudo: sudo sh experiments.sh' >&2; exit 1; }
[ -x "$binary" ] || { echo "Put the ARM64 binary named vft-stream next to $0" >&2; exit 1; }

tracker() {
  for device in /sys/bus/usb/devices/*; do
    [ "$(cat "$device/idVendor" 2>/dev/null)" = 0bb4 ] &&
      [ "$(cat "$device/idProduct" 2>/dev/null)" = 06a1 ] && { echo "$device"; return 0; }
  done
  return 1
}

drivers() {
  device=$(tracker) || { echo 'tracker not found'; return; }
  printf '%s speed=%s Mbit/s\n' "$device" "$(cat "$device/speed" 2>/dev/null)"
  for interface in "$device":*; do
    [ -d "$interface" ] || continue
    printf '  %s driver=%s\n' "${interface##*/}" "$(basename "$(readlink "$interface/driver" 2>/dev/null)" 2>/dev/null)"
  done
  for node in /sys/class/video4linux/video*; do
    [ -e "$node" ] && printf '  %s -> %s (%s)\n' "${node##*/}" "$(readlink -f "$node/device")" "$(cat "$node/name" 2>/dev/null)"
  done
}

reenumerate() {
  device=$(tracker) || { echo 'tracker not found; reconnect it'; return 1; }
  echo 0 > "$device/authorized" && sleep 2 && echo 1 > "$device/authorized"
  # uvcvideo needs a moment to bind and create /dev/video* again.
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    tracker >/dev/null && ls /dev/video* >/dev/null 2>&1 && break
    sleep 1
  done
  sleep 2
}

variant() {
  name=$1; shift
  printf '\n===== %s: vft-stream --tracker focus3 --startup-timeout 8 %s =====\n' "$name" "$*"
  reenumerate || return
  drivers
  timeout -s INT 15 "$binary" --tracker focus3 --startup-timeout 8 "$@" 2>&1
  printf -- '-- exit status %s\n' "$?"
}

{
  printf '== system ==\n'
  uname -a
  date
  printf '\n== uvcvideo module ==\n'
  if [ -d /sys/module/uvcvideo ]; then
    echo 'uvcvideo loaded'
    for parameter in /sys/module/uvcvideo/parameters/*; do
      [ -r "$parameter" ] && printf '  %s=%s\n' "${parameter##*/}" "$(cat "$parameter")"
    done
  else
    echo 'uvcvideo not loaded; trying modprobe'
    modprobe uvcvideo 2>&1 && echo 'modprobe uvcvideo: ok'
  fi
  printf '\n== tracker before experiments ==\n'
  drivers
  # Kernel driver first: it is the stack the Focus 3 headset itself uses.
  variant A-uvcvideo --capture-backend v4l2
  variant B-default
  variant C-no-clear-halt --no-clear-halt
  variant D-libuvc --capture-backend libuvc
  variant E-capture-first --capture-first
  variant F-capture-first-no-halt --capture-first --no-clear-halt
  printf '\n== kernel messages ==\n'
  dmesg 2>/dev/null | grep -iE 'uvc|usb [0-9-]+.*(0bb4|htc|lip|reset|disconnect|new high)' | tail -n 80
} > "$report" 2>&1
printf 'Wrote %s\n' "$report"
