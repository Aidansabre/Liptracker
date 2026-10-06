#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
root=${LIPTRACKER_BUILD_ROOT:-"$repo/.build"}
prefix="$root/native/prefix"
sources="$root/sources"
tools=${LIPTRACKER_TOOLS:-"$root/tools"}
if [ ! -f "$tools/PIL/__init__.py" ]; then
  python3 -m pip install --target "$tools" pillow==11.3.0
fi
[ -f "$prefix/include/libuvc/libuvc.h" ] || { echo 'Run scripts/build.sh native first.' >&2; exit 1; }
temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT HUP INT TERM
cc -O2 -Wall -Wextra -Werror -I"$prefix/include" -I"$prefix/include/libusb-1.0" \
  -I"$sources/stb-2c980bb59875b0d32144a71867fbdebb2f77cd20" \
  "$repo/vft-stream.c" "$repo/tracker.c" "$repo/image.c" "$repo/tests/fake_uvc.c" \
  -lm -lpthread -o "$temp/vft-stream-simulated"
PYTHONPATH="$tools${PYTHONPATH:+:$PYTHONPATH}" python3 "$repo/tests/test_integration.py" "$temp/vft-stream-simulated"
