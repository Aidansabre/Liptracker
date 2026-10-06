Focus 3 direct bulk capture for Steam Frame.

The earlier libuvc capture stalled, but the direct USB probe received camera
payloads. This release uses direct bulk reads and UVC frame assembly by default
for Focus 3. It retains partial data returned with timeouts, recovers after
incomplete/error frames, and reports frame sizes and UVC error counts. The
original VFT capture path is preserved.

A thorough review of Kirisame-Nanoha/Lip_Camera_IP_Server confirmed that our
five activation/shutdown commands, padding and delays match exactly. Their
camera code selects the reported YUY2 format; 320×480 is resized network output.
We retain the tracker's advertised 640×481 at approximately 30 FPS. The review
is included in `docs/focus3-reference-review.md`.

Download `vft-stream-focus3-steam-frame.tar.gz` for the static ARM64 executable,
helper scripts, complete application source and licenses. `SHA256SUMS` verifies
the package and standalone executable. Stop other camera processes, then run:

```sh
tar -xzf vft-stream-focus3-steam-frame.tar.gz
cd vft-stream-focus3
sudo ./vft-stream --tracker focus3 2>focus3-capture.log
cat focus3-capture.log
```

Connect to `http://<Steam-Frame-IP>:8085/` for MJPEG output. If capture fails,
share the complete log, including `bulk frame` and `bulk totals` lines. A full
640×481 YUY2 image is 615680 bytes; a 640×480 image would be 614400 bytes.
Incomplete and error-marked frames are rejected by default.

Useful comparison options:

- `--capture-first`: queue capture before the existing activation commands.
- `--capture-backend libuvc`: use the earlier capture implementation.
- `--allow-uvc-errors -r`: inspect full-size UVC ERR frames without processing;
  incomplete frames remain rejected.
- `--probe-bulk --startup-timeout 5`: inspect raw transfers without MJPEG.

Native and static ARM64 builds pass. Protocol, image and payload-decoder tests
pass with address/undefined-behavior sanitizers, as do nineteen simulated
USB/HTTP integration tests. Original VFT image output matches the upstream
fixture byte for byte.

This is a **hardware-test prerelease**. Successful streaming on Steam Frame,
actual frame dimensions/error rates, IR operation, orientation, sustained FPS,
Baballonia tracking quality and service plug/unplug behavior remain unverified.
