Focus 3: kernel uvcvideo capture and a one-run experiment script.

The `.6` hardware run showed a high-speed USB link (480 Mbit/s) and accepted
all vendor commands, but no image data arrived after the commit. No run has yet
made the sensor produce frames through libuvc/libusb.

Changes:

- `--capture-backend v4l2` captures through the kernel `uvcvideo` driver: the
  Linux host stack the Focus 3 headset itself uses. XU commands go through
  `UVCIOC_CTRL_QUERY`; `STREAMON` commits after activation and queues bulk reads.
- `--no-clear-halt` separates the effect of the `.6` endpoint halt.
- `experiments.sh` runs six capture variants in one go and writes `experiments.txt`.

Unplug/reconnect the tracker once, then in the extracted package folder:

```sh
sudo sh experiments.sh
cat experiments.txt
```

It takes about two minutes. Share the complete `experiments.txt`.

Native and static ARM64 builds, protocol/image/payload regressions and
26 simulated USB/HTTP integration tests pass, including sanitizer checks.
The uvcvideo path is compile-checked and validated for setup failures only;
it needs the physical tracker. Published hashes are in `SHA256SUMS`.

This is a hardware-test prerelease.
