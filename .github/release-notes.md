Vive Focus 3 Facial Tracker support for vft-stream on Steam Frame.

- Detects `0bb4:06a1` / `HTC Lip Camera` and discovers its vendor extension unit.
- Uses the Focus 3 stream/IR sequence with SET_CUR-only writes.
- Captures the reported 640×481 YUY2 bulk mode and serves the full luminance
  image as a rotated 320×480 MJPEG stream for Baballonia.
- Preserves the original Vive Facial Tracker's image processing.
- Includes raw/rotation options, descriptor diagnostics, shutdown cleanup,
  and an optional udev/systemd installer.

Download `vft-stream-focus3-steam-frame.tar.gz` for the binary, helper scripts,
full application source and license notices. The standalone
`vft-stream-steam-frame` executable is static ARM64/musl. `SHA256SUMS` verifies
both downloads.

```sh
tar -xzf vft-stream-focus3-steam-frame.tar.gz
cd vft-stream-focus3
sudo ./vft-stream --tracker focus3 --diagnose
sudo ./vft-stream --tracker focus3
```

Point Baballonia's Wireless/IP Camera input at `http://<Steam-Frame-IP>:8085/`.
Use `--rotation 0|90|180|270` if needed, or `-r` to inspect the full raw frame.

The release workflow builds both native and static ARM64 binaries, runs the
protocol/image tests and sanitizer checks, and runs seven simulated USB/HTTP
integration tests. The original VFT image fixture matches upstream byte for byte.

This is a **prerelease for hardware testing**. Physical USB transfers, IR
operation, orientation, sustained FPS, Baballonia tracking quality and service
plug/unplug behavior still require validation on Steam Frame.
