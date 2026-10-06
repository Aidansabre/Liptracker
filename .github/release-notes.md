Focus 3 capture startup update for Steam Frame.

The first hardware test of `v0.3.0-focus3.1` opened the tracker and completed
vendor writes, but exited without a usable frame. This update commits the UVC
capture mode **before** sensor/IR activation, then starts transfers without
another mode commit. This ordering change still needs validation on the tracker.

- Initializes the UVC control block before negotiation.
- Allows ten seconds for the first usable frame; retains the three-second
  watchdog after capture begins. Use `--startup-timeout 1–120` to adjust startup.
- Logs negotiated UVC frame/payload sizes and the first callback's geometry,
  stride and byte count.
- Reports received/rejected/encoded counts on timeout to distinguish missing
  callbacks from rejected image data.

Focus 3 support uses `0bb4:06a1`, the reported 640×481 YUY2 bulk mode and
SET_CUR-only activation. Output is a rotated 320×480 luminance MJPEG stream.
Raw/rotation options, descriptor diagnostics, shutdown cleanup and the optional
service installer are included. Original VFT image processing is preserved.

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

If capture still fails, paste the complete application output, including the
negotiated UVC values and timeout counters:

```sh
sudo ./vft-stream --tracker focus3 2>focus3-capture.log
cat focus3-capture.log
```

The release workflow builds both native and static ARM64 binaries, runs the
protocol/image tests and sanitizer checks, and runs ten simulated USB/HTTP
integration tests. These include commit/activation ordering, delayed startup,
missing callbacks, short frames and failure cleanup. The original VFT image
fixture matches upstream byte for byte.

This is a **prerelease for hardware testing**. Physical USB transfers, IR
operation, orientation, sustained FPS, Baballonia tracking quality and service
plug/unplug behavior still require validation on Steam Frame.
