#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
[ -x "$repo/dist/vft-stream-steam-frame" ] || { echo 'Run scripts/build.sh steam-frame first.' >&2; exit 1; }
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/vft-stream-focus3/source"
cp "$repo/dist/vft-stream-steam-frame" "$stage/vft-stream-focus3/vft-stream"
cp "$repo/diag.sh" "$repo/experiments.sh" "$repo/install-service.sh" "$repo/README.md" "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$stage/vft-stream-focus3/"
cp -R "$repo/licenses" "$stage/vft-stream-focus3/"
cp -R "$repo/docs" "$stage/vft-stream-focus3/"
for item in vft-stream.c tracker.c tracker.h image.c image.h bulk_capture.c bulk_capture.h uvc_bulk.c uvc_bulk.h package.nix default.nix scripts tests docs; do
  cp -R "$repo/$item" "$stage/vft-stream-focus3/source/"
done
cp "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$repo/README.md" "$stage/vft-stream-focus3/source/"
cp -R "$repo/licenses" "$stage/vft-stream-focus3/source/"
(cd "$stage/vft-stream-focus3"; sha256sum vft-stream > SHA256SUMS)
tar -czf "$repo/dist/vft-stream-focus3-steam-frame.tar.gz" -C "$stage" vft-stream-focus3
printf 'Packaged %s\n' "$repo/dist/vft-stream-focus3-steam-frame.tar.gz"
