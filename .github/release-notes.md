Focus 3: SET_INTERFACE before the format probe and a one-run experiment script.

Supersedes `.7`, whose kernel `uvcvideo` mode cannot run on Steam Frame: its
kernel has no `uvcvideo` driver. That mode has been removed.

The `.6` hardware run showed a high-speed USB link (480 Mbit/s) and accepted
all vendor commands, but no image data arrived after the commit.

Changes:

- Focus 3 startup selects alternate setting 0 on the streaming interface
  (SET_INTERFACE) before the format probe, as Linux uvcvideo does at bind time.
  libuvc never sends it for bulk cameras.
- `--no-clear-halt` separates the effect of the `.6` endpoint halt.
- `experiments.sh` runs five capture variants in one go, re-enumerating the
  tracker through sysfs between them, and writes `experiments.txt`.

Unplug/reconnect the tracker once, then in the extracted package folder:

```sh
sudo sh experiments.sh
cat experiments.txt
```

It takes about two minutes. Share the complete `experiments.txt`.

Native and static ARM64 builds, protocol/image/payload regressions and
25 simulated USB/HTTP integration tests pass, including sanitizer checks.
Published hashes are in `SHA256SUMS`.

This is a hardware-test prerelease.
